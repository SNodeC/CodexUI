// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ipc/SNodeSocketPairEndpoint.h"
#include "codex/ipc/SocketPair.h"

#include <core/SNodeC.h>
#include <utils/Timeval.h>

#include "codex/ipc/QtSocketPairEndpoint.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QTimer>

#include <atomic>
#include <array>
#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr std::size_t MaximumQueuedBytes = 64;
constexpr std::size_t MaximumReadBytesPerEvent = 64U * 1024U;
constexpr std::string_view FromQt = "qt-frame-1\nqt-frame-2\n";
constexpr std::string_view FromSNode = "snode-frame-1\nsnode-frame-2\n";
constexpr std::string_view Acknowledgement = "snode-ack\n";

bool expect(bool condition, const char *message) {
  std::cout << (condition ? "PASS " : "FAIL ") << message << '\n';
  return condition;
}

bool qtPartialWritesRetainOnlyQueuedBytes() {
  constexpr std::size_t QueueLimit = 32U * 1024U;
  codexui::codex::ipc::SocketPair pair;
  if (!pair.isValid())
    return false;
  const int qtDescriptor = pair.releaseFirstEndpoint();
  const int peerDescriptor = pair.releaseSecondEndpoint();
  int socketBytes = 4096;
  static_cast<void>(::setsockopt(qtDescriptor, SOL_SOCKET, SO_SNDBUF,
                                &socketBytes, sizeof(socketBytes)));
  codexui::codex::ipc::QtSocketPairEndpoint endpoint(
      qtDescriptor, QueueLimit, 64U * 1024U, 257);
  const std::string chunk(2048, 'q');
  std::array<char, 4096> drain{};
  bool bounded = true;
  for (int round = 0; round < 512; ++round) {
    if (!endpoint.send(chunk)) {
      while (::recv(peerDescriptor, drain.data(), drain.size(), MSG_DONTWAIT) >
             0) {
      }
      QCoreApplication::processEvents();
      static_cast<void>(endpoint.send(chunk));
    }
    bounded &= endpoint.retainedWriteBytes() <= QueueLimit + chunk.size();
    if (round % 4 == 0) {
      static_cast<void>(
          ::recv(peerDescriptor, drain.data(), drain.size(), MSG_DONTWAIT));
      QCoreApplication::processEvents();
    }
  }
  endpoint.close();
  ::close(peerDescriptor);
  return bounded;
}

} // namespace

int main(int argc, char *argv[]) {
  QCoreApplication application(argc, argv);
  core::SNodeC::init(argc, argv);

  bool passed = expect(qtPartialWritesRetainOnlyQueuedBytes(),
                       "Qt partial writes retain only bounded queued bytes");

  codexui::codex::ipc::SocketPair pair;
  if (!expect(pair.isValid(), "nonblocking Unix socketpair is created"))
    return 1;

  codexui::codex::ipc::QtSocketPairEndpoint qtEndpoint(
      pair.releaseFirstEndpoint(), MaximumQueuedBytes);
  const int snodeDescriptor = pair.releaseSecondEndpoint();

  std::atomic_bool snodeCreated = false;
  std::atomic_bool snodeBounded = false;
  std::atomic_bool snodeReceived = false;
  std::atomic_bool snodeClosed = false;
  std::atomic_bool snodeError = false;
  std::atomic_int eventLoopResult = -1;
  std::string receivedBySNode;
  std::promise<void> snodeReady;
  std::future<void> ready = snodeReady.get_future();

  std::thread snodeThread([&] {
    auto *endpoint = codexui::codex::ipc::SNodeSocketPairEndpoint::create(
        snodeDescriptor, MaximumQueuedBytes, MaximumReadBytesPerEvent);
    snodeCreated = endpoint != nullptr;
    if (!endpoint) {
      snodeReady.set_value();
      QMetaObject::invokeMethod(&application, &QCoreApplication::quit,
                                Qt::QueuedConnection);
      return;
    }

    endpoint->setOnData([&, endpoint](const char *data, std::size_t size) {
      receivedBySNode.append(data, size);
      if (!snodeReceived && receivedBySNode == FromQt) {
        snodeReceived = true;
        static_cast<void>(
            endpoint->send(Acknowledgement.data(), Acknowledgement.size()));
      }
    });
    endpoint->setOnError([&](int) { snodeError = true; });
    endpoint->setOnClosed([&] {
      snodeClosed = true;
      core::SNodeC::stop();
      QMetaObject::invokeMethod(&application, &QCoreApplication::quit,
                                Qt::QueuedConnection);
    });

    const std::string oversized(MaximumQueuedBytes + 1, 'x');
    snodeBounded = !endpoint->send(oversized);
    static_cast<void>(endpoint->send(FromSNode.substr(0, 14).data(), 14));
    static_cast<void>(
        endpoint->send(FromSNode.substr(14).data(), FromSNode.size() - 14));
    snodeReady.set_value();
    eventLoopResult = core::SNodeC::start(utils::Timeval({5, 0}));
  });

  if (ready.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    core::SNodeC::stop();
    snodeThread.join();
    expect(false, "SNode.C socketpair endpoint becomes ready");
    return 1;
  }

  std::string receivedByQt;
  bool qtSent = false;
  bool qtReceived = false;
  bool qtClosed = false;
  bool qtError = false;
  qtEndpoint.setOnData([&](const char *data, std::size_t size) {
    receivedByQt.append(data, size);
    if (!qtSent && receivedByQt.starts_with(FromSNode)) {
      qtSent = qtEndpoint.send(FromQt.substr(0, 11).data(), 11) &&
               qtEndpoint.send(FromQt.substr(11).data(), FromQt.size() - 11);
    }
    if (receivedByQt == std::string(FromSNode) + std::string(Acknowledgement)) {
      qtReceived = true;
      qtEndpoint.close();
    }
  });
  qtEndpoint.setOnError([&](int) { qtError = true; });
  qtEndpoint.setOnClosed([&] { qtClosed = true; });

  const std::string oversized(MaximumQueuedBytes + 1, 'x');
  const bool qtBounded = !qtEndpoint.send(oversized);

  QTimer::singleShot(5000, &application, [&] {
    core::SNodeC::stop();
    application.quit();
  });
  application.exec();
  core::SNodeC::stop();
  if (qtEndpoint.isOpen())
    qtEndpoint.close();
  snodeThread.join();

  passed &= expect(snodeCreated, "SNode.C endpoint is created");
  passed &= expect(qtBounded && snodeBounded,
                   "both endpoints reject writes beyond their queue bound");
  passed &= expect(qtSent && snodeReceived && receivedBySNode == FromQt,
                   "Qt-to-SNode.C frames preserve byte order");
  passed &=
      expect(qtReceived && receivedByQt == std::string(FromSNode) +
                                               std::string(Acknowledgement),
             "SNode.C-to-Qt frames preserve byte order");
  passed &= expect(qtClosed && snodeClosed,
                   "closing one endpoint cleanly closes both sides");
  passed &= expect(!qtError && !snodeError,
                   "normal exchange and shutdown report no transport error");
  passed &= expect(eventLoopResult == 0, "SNode.C event loop exits cleanly");
  return passed ? 0 : 1;
}
