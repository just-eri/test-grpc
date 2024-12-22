#include <net/if.h>
#include <sys/ioctl.h>

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWidget>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_format.h"
#include "api.grpc.pb.h"

#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
constexpr int BROADCAST_PORT = 10001;

ABSL_FLAG(uint16_t, port, 50051, "Server port for the service");

class MaintainingApiServiceImpl final : public MaintainingApi::Service {
  Status Ping(ServerContext* context, const PingRequest* request,
              PingResponse* reply) override {
    last_ping = std::chrono::steady_clock::now();
    ip = request->clientip();
    return Status::OK;
  }

 public:
  std::string ip;
  std::chrono::steady_clock::time_point last_ping;
};

std::string get_local_ip() {
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    perror("Socket creation failed");
    return "127.0.0.1";
  }

  char buffer[1024];
  struct ifconf ifc {};
  ifc.ifc_len = sizeof(buffer);
  ifc.ifc_buf = buffer;

  if (ioctl(sock, SIOCGIFCONF, &ifc) == -1) {
    perror("ioctl error");
    close(sock);
    return "127.0.0.1";
  }

  struct ifreq* it = ifc.ifc_req;
  const struct ifreq* const end = it + (ifc.ifc_len / sizeof(struct ifreq));
  std::string ip_address = "127.0.0.1";

  for (; it != end; ++it) {
    struct sockaddr_in* addr = (struct sockaddr_in*)&it->ifr_addr;

    if (addr->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
      continue;
    }

    ip_address = inet_ntoa(addr->sin_addr);
    break;
  }

  close(sock);
  return ip_address;
}

class SimpleApp : public QWidget {
  Q_OBJECT

 public:
  SimpleApp(QWidget* parent = nullptr) : QWidget(parent) {
    connected = false;
    auto* layout = new QVBoxLayout(this);

    textEdit = new QTextEdit(this);
    textEdit->setReadOnly(true);
    layout->addWidget(textEdit);

    QHBoxLayout* hLayout = new QHBoxLayout();
    QLabel* label = new QLabel("Port", this);
    hLayout->addWidget(label);
    inputLine = new QLineEdit(this);
    hLayout->addWidget(inputLine);
    layout->addLayout(hLayout);

    auto* button = new QPushButton("Start", this);
    layout->addWidget(button);

    connect(button, &QPushButton::clicked, this, &SimpleApp::onAddTextClicked);
  }

 private slots:
  void RunServer() {
    std::string server_address = absl::StrFormat("0.0.0.0:%d", srvPort);
    MaintainingApiServiceImpl service;

    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    ServerBuilder builder;

    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

    builder.RegisterService(&service);

    grpc::CompletionQueue cq;

    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;

    auto last_ping = service.last_ping;
    while (true) {
      if (service.last_ping != last_ping) {
        if (!connected) {
          textEdit->append(QString::fromStdString(service.ip) +
                           " connected...");
          connected = true;
        }
        textEdit->append("Ping!");
        last_ping = service.last_ping;
      } else {
        auto now = std::chrono::steady_clock::now();
        auto diff =
            std::chrono::duration_cast<std::chrono::seconds>(now - last_ping)
                .count();
        if (diff >= 15 && connected && !service.ip.empty()) {
          textEdit->append(QString::fromStdString(service.ip) +
                           " disconnected...");
          connected = false;
        }
      }
    }
  }

  void udp_broadcast(const std::string& message) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
      perror("Socket creation failed");
      return;
    }

    int broadcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast,
                   sizeof(broadcast)) < 0) {
      perror("Failed to set broadcast option");
      ::close(sock);
      return;
    }

    struct sockaddr_in broadcast_addr {};
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(BROADCAST_PORT);
    broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    if (sendto(sock, message.c_str(), message.size(), 0,
               (struct sockaddr*)&broadcast_addr, sizeof(broadcast_addr)) < 0) {
      perror("Broadcast failed");
    } else {
      std::cout << "Broadcasted message: " << message << std::endl;
    }

    ::close(sock);
  }

  void onAddTextClicked() {
    QString port = inputLine->text();
    if (port.isEmpty()) {
      QMessageBox::warning(this, "Warning", "Port is empty!");
      return;
    }
    bool ok;
    srvPort = port.toUShort(&ok);
    if (ok && srvPort > 0 && srvPort < 65536) {
      auto ip = get_local_ip();
      std::thread udp_broadcaster([=]() {
        while (true) {
          if (connected) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
          }
          udp_broadcast(ip + ":" + port.toStdString());
          std::this_thread::sleep_for(std::chrono::seconds(10));
        }
      });
      udp_broadcaster.detach();
      std::thread(&SimpleApp::RunServer, this).detach();
    } else {
      QMessageBox::warning(this, "Warning", "Port is invalid!");
      return;
    }

    inputLine->clear();
  }

 private:
  QLineEdit* inputLine;
  QTextEdit* textEdit;
  uint16_t srvPort;
  bool connected;
};

#include "testgrpc_server.moc"

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  auto port = absl::GetFlag(FLAGS_port);
  auto ip = get_local_ip();

  QApplication app(argc, argv);

  SimpleApp window;
  window.setWindowTitle("gRPC Server");
  window.resize(400, 300);
  window.show();

  return app.exec();
}
