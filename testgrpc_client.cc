#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <QApplication>
#include <QColor>
#include <QHBoxLayout>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "api.grpc.pb.h"

#include <grpcpp/grpcpp.h>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;

constexpr int BROADCAST_PORT = 10001;

struct server_status_t {
  std::time_t last_ping;
  bool status;
  int failed_pings;
};
using server_map = std::map<std::string, server_status_t>;
using smap_ptr = std::shared_ptr<server_map>;

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

class MaintainingApiClient {
 public:
  MaintainingApiClient(std::shared_ptr<Channel> channel)
      : stub_(MaintainingApi::NewStub(channel)) {}

  std::string Ping(const std::string& clientIp) {
    PingRequest request;
    request.set_clientip(clientIp);

    PingResponse reply;

    ClientContext context;

    Status status = stub_->Ping(&context, request, &reply);

    if (status.ok()) {
      return "Ok";
    } else {
      std::cout << status.error_code() << ": " << status.error_message()
                << std::endl;
      return "RPC failed";
    }
  }

 private:
  std::unique_ptr<MaintainingApi::Stub> stub_;
};

class SimpleApp : public QWidget {
 public:
  SimpleApp(QWidget* parent = nullptr) : QWidget(parent) {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    tableWidget = new QTableWidget(this);
    tableWidget->setColumnCount(4);
    tableWidget->setHorizontalHeaderLabels(
        {"IP", "Last ping time", "Status", "Action"});

    updateTable();

    mainLayout->addWidget(tableWidget);

    setLayout(mainLayout);
    std::thread(&SimpleApp::udp_receive, this).detach();
  }

 public slots:
  QString getFormattedTime(time_t time) {
    if (time == 0) {
      return "-";
    }

    struct tm* timeInfo = localtime(&time);

    QTime timeObj(timeInfo->tm_hour, timeInfo->tm_min, timeInfo->tm_sec);

    return timeObj.toString("hh:mm");
  }

  void udp_receive() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
      perror("Socket creation failed");
      return;
    }

    struct sockaddr_in recv_addr {};
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_port = htons(BROADCAST_PORT);
    recv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr*)&recv_addr, sizeof(recv_addr)) < 0) {
      perror("Bind failed");
      ::close(sock);
      return;
    }

    char buffer[1024];
    while (true) {
      ssize_t len = recvfrom(sock, buffer, sizeof(buffer), 0, nullptr, nullptr);
      if (len > 0) {
        auto recv_str = std::string(buffer, len);
        std::cout << "Received: " << recv_str << std::endl;
        servers.try_emplace(recv_str, server_status_t{0, false, 0});
        updateTable();
      }
    }

    ::close(sock);
  }

  void updateTable() {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          tableWidget->setRowCount(0);

          for (auto& [key, value] : servers) {
            int row = tableWidget->rowCount();
            tableWidget->insertRow(row);

            tableWidget->setItem(
                row, 0, new QTableWidgetItem(QString::fromStdString(key)));
            tableWidget->setItem(
                row, 1,
                new QTableWidgetItem(getFormattedTime(value.last_ping)));
            tableWidget->setItem(
                row, 2,
                new QTableWidgetItem(value.status ? "Online" : "Offline"));

            QPushButton* button = new QPushButton("Connect", this);
            tableWidget->setCellWidget(row, 3, button);
            if (value.status) {
              tableWidget->item(row, 2)->setForeground(Qt::green);
              button->setText("Disconnect");
            } else {
              tableWidget->item(row, 2)->setForeground(Qt::red);
              button->setText("Connect");
            }

            connect(button, &QPushButton::clicked, [this, row, key]() {
              QPushButton* btn =
                  qobject_cast<QPushButton*>(tableWidget->cellWidget(row, 3));
              if (btn) {
                if (btn->text() == "Connect") {
                  btn->setText("Disconnect");
                  servers[key].status = true;
                  tableWidget->item(row, 2)->setForeground(Qt::green);
                  tableWidget->item(row, 2)->setText("Online");
                  startPings(key);
                } else {
                  btn->setText("Connect");
                  servers[key].status = false;
                  tableWidget->item(row, 2)->setForeground(Qt::red);
                  tableWidget->item(row, 2)->setText("Offline");
                  stopPings(key);
                }
              }
            });
          }
        },
        Qt::QueuedConnection);
  }

  void startPings(std::string key) {
    std::thread pinger([=]() {
      MaintainingApiClient client(
          grpc::CreateChannel(key, grpc::InsecureChannelCredentials()));
      while (true) {
        if (!servers[key].status) break;
        auto response = client.Ping(get_local_ip());
        if (response == "Ok") {
          servers[key].failed_pings = 0;
          servers[key].last_ping = std::time(nullptr);
          updateTable();
        } else {
          servers[key].failed_pings++;
          updateTable();
        }
        if (servers[key].failed_pings >= 3) {
          stopPings(key);
          break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
      }
    });
    pinger.detach();
  }

  void stopPings(std::string key) {
    servers[key].status = false;
    servers[key].failed_pings = 0;
    updateTable();
  }

 private:
  QTableWidget* tableWidget;
  server_map servers;
};

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);

  server_map servers{};
  auto map_ptr = std::shared_ptr<server_map>{&servers};

  QApplication app(argc, argv);

  SimpleApp window;
  window.setWindowTitle("Qt Table with Button and Colored Text");
  window.resize(400, 200);
  window.show();

  return app.exec();
}
