#pragma once
#include <stdint.h>    // for uint8_t, uint16_t
#include <functional>  // for function
#include <string>      // for string

#include "TCPSocket.h"  // for TCPSocket
#include "mqtt.h"       // for MQTT_PUBLISH_QOS_0, MQTT_PUBLISH_QOS_1, MQTT_...

namespace bell {
/// MQTTClient is a thin wrapper around the MQTT client library.
class MQTTClient {
 public:
  MQTTClient(){};
  ~MQTTClient() = default;

  enum class QOS {
    AT_MOST_ONCE = MQTT_PUBLISH_QOS_0,
    AT_LEAST_ONCE = MQTT_PUBLISH_QOS_1,
    EXACTLY_ONCE = MQTT_PUBLISH_QOS_2
  };

  // @brief Callback for when a message is published.
  // @param topic The topic the message was published to.
  // @param message The message that was published.
  typedef std::function<void(const std::string& topic,
                             const std::string& message)>
      PublishCallback;

  // @brief Set the publish callback
  void setPublishCallback(PublishCallback callback) {
    _publishCallback = callback;
  }

  // @brief Connect to an MQTT broker.
  // @param host The host to connect to.
  // @param port The port to connect to.
  // @param username The username to authenticate with.
  // @param password The password to authenticate with.
  void connect(const std::string& host, uint16_t port,
               const std::string& username = "",
               const std::string& password = "");

  // @brief Disconnect from the MQTT broker.
  void disconnect();

  // @brief Synchronize with the MQTT broker.
  void sync();

  // @brief Publish a message to a topic.
  // @param topic The topic to publish to.
  // @param message The message to publish.
  // @param qos The quality of service to publish with.
  void publish(const std::string& topic, const std::string& message,
               QOS qos = QOS::AT_MOST_ONCE);

  // @brief Subscribe to a topic.
  // @param topic The topic to subscribe to.
  void subscribe(const std::string& topic, QOS qos = QOS::AT_MOST_ONCE);

  // @brief Unsubscribe from a topic.
  // @param topic The topic to unsubscribe from.
  void unsubscribe(const std::string& topic);

  // @brief Check if the MQTT client is connected.
  // @retreturn True if the MQTT client is connected, false otherwise.
  bool isConnected();

  // Directly mapped from mqtt client's publish_callback field
  void publishCallback(struct mqtt_response_publish* published);

 private:
  bell::TCPSocket socket;
  bool connected = false;
  PublishCallback _publishCallback = nullptr;

  // mqtt lib internals
  struct mqtt_client client;
  uint8_t sendbuf[2048];
  uint8_t recvbuf[1024];
};
}  // namespace bell
