#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <string>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <rx.hpp>

#include <ccurl/ccurl.h>
#include <iota/utils/common/api.hpp>
#include <iota/utils/common/iri.hpp>
#include <iota/utils/common/zmqpub.hpp>

#include <prometheus/exposer.h>
#include <prometheus/registry.h>

DEFINE_string(zmqURL, "tcp://m5.iotaledger.net:5556",
              "URL of ZMQ publisher to connect to");

DEFINE_string(iriHost, "http://node02.iotatoken.nl:14265",
              "URL of IRI API to use");

DEFINE_string(prometheusExposerIP, "0.0.0.0:8080",
              "URL of the prometheus exposer from where its data will be scraped");

DEFINE_int32(mwm, 14, "Minimum Weight Magnitude");

using namespace iota::utils;

const std::string TX_TRYTES =
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999SAYHELLOTOECHOCATCHERECHOCATCHING"
    "SINCETWENTYSEVENTEENONTHEIOTATANGLE999999999999999999999999999999999999999"
    "9VD9999999999999999999999999JAKBHNJIE999999999999999999JURSJVFIECKJYEHPATC"
    "XADQGHABKOOEZCRUHLIDHPNPIGRCXBFBWVISWCF9ODWQKLXBKY9FACCKVXRAGZ999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "99999999999999999999999999999999999999999999999999999999999999999999999999"
    "999999999";

struct HashedTX {
  std::string hash;
  std::string tx;
};

std::string fillTX(api::GetTransactionsToApproveResponse response) {
  std::string tx = TX_TRYTES;

  tx.replace(2430, 81, response.trunkTransaction);
  tx.replace(2511, 81, response.branchTransaction);

  return std::move(tx);
}

pplx::task<std::string> constructTXs(api::IRIClient* client) {
  return client->getTransactionsToApprove().then(fillTX);
}

std::string powTX(std::string tx, int mwm) { return ccurl_pow(tx.data(), mwm); }

HashedTX hashTX(std::string tx) {
  return {ccurl_digest_transaction(tx.data()), std::move(tx)};
}

int main(int argc, char** argv) {
  ::gflags::ParseCommandLineFlags(&argc, &argv, true);
  ::google::InitGoogleLogging("echocatcher");
  using namespace prometheus;

  LOG(INFO) << "Booting up.";
  ccurl_pow_init();

  LOG(INFO) << "IRI Host: " << FLAGS_iriHost;

  auto iriClient = std::make_unique<api::IRIClient>(FLAGS_iriHost);

  auto task =
      iriClient->getTransactionsToApprove()
          .then(fillTX)
          .then([](std::string tx) { return powTX(std::move(tx), FLAGS_mwm); })
          .then(hashTX);

  task.wait();
  auto hashed = task.get();
  LOG(INFO) << "Hash: " << hashed.hash;

  Exposer exposer{FLAGS_prometheusExposerIP};

  // create a metrics registry with component=main labels applied to all its
  // metrics
  auto registry = std::make_shared<Registry>();

  auto& gauge_received_family = BuildGauge()
            .Name("time_elapsed_received")
            .Help("#Milli seconds it took for tx to travel back to transaction's original source(\"listen_node\")")
            .Labels({{"publish_node",FLAGS_zmqURL},{"listen_node",FLAGS_iriHost}})
            .Register(*registry);

  auto& gauge_arrived_family = BuildGauge()
            .Name("time_elapsed_arrived")
            .Help("#Milli seconds it took for tx to arrive to destination (\"publish_node\")")
            .Labels({{"publish_node",FLAGS_zmqURL},{"listen_node",FLAGS_iriHost}})
            .Register(*registry);

  exposer.RegisterCollectable(registry);

  auto zmqThread = rxcpp::schedulers::make_new_thread();
  auto zmqObservable =
      rxcpp::observable<>::create<std::shared_ptr<iri::IRIMessage>>(
          [&](auto s) { zmqPublisher(std::move(s), FLAGS_zmqURL); })
          .observe_on(rxcpp::observe_on_new_thread());

  auto broadcast = iriClient->broadcastTransactions({hashed.tx});
  auto start = std::chrono::system_clock::now();

  zmqObservable.observe_on(rxcpp::synchronize_new_thread())
      .subscribe(
          [start, hashed, &gauge_received_family,
           &gauge_arrived_family](std::shared_ptr<iri::IRIMessage> msg) {
            if (msg->type() != iri::IRIMessageType::TX) return;

            auto tx = std::static_pointer_cast<iri::TXMessage>(std::move(msg));

            if (tx->hash() == hashed.hash) {
              auto& current_received_duration_gauge = gauge_received_family.Add(
                  {{"bundle_size", std::to_string(tx->lastIndex() + 1)}});

              auto& current_arrived_duration_gauge = gauge_arrived_family.Add(
                  {{"bundle_size", std::to_string(tx->lastIndex() + 1)}});

              auto received = std::chrono::system_clock::now();
              auto elapsed_until_received =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      received - start)
                      .count();

              auto elapsed_until_arrived =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      tx->arrivalTime() - start)
                      .count();

              current_received_duration_gauge.Set(elapsed_until_received);
              current_arrived_duration_gauge.Set(elapsed_until_arrived);
            }
          },
          []() {});

  return 0;
}
