/**
 * wsserver
 * Server.cpp
 *
 * @author Eduard Maximovich <edward.vstock@gmail.com>
 * @link https://github.com/edwardstock
 */

#include <fmt/format.h>
#include "ChatServer.h"
#include "../helpers/helpers.h"
#include "../base/Settings.hpp"

#ifdef USE_SECURE_SERVER
wss::ChatServer::ChatServer(
    const std::string &crtPath, const std::string &privKeyPath,
    const std::string &host, unsigned short port, const std::string &regexPath) :
    m_useSSL(true),
    m_maxMessageSize(10 * 1024 * 1024),
    m_server(std::make_unique<WsServer>(crtPath, privKeyPath)),
    m_connectionStorage(std::make_unique<wss::ConnectionStorage>()) {
    m_server->config.port = port;
    m_server->config.threadPoolSize = std::thread::hardware_concurrency();
    m_server->config.maxMessageSize = m_maxMessageSize;

    if (host.length() >= 7) {
        m_server->config.address = host;
    };

    m_endpoint = &m_server->endpoint[regexPath];
    m_endpoint->onMessage = [this](WsConnectionPtr connectionPtr, WsMessagePtr messagePtr) {
      if (messagePtr->fin_rsv_opcode == FLAG_PONG) {
          onPong(connectionPtr, messagePtr);
          return;
      }
      onMessage(connectionPtr, messagePtr);
    };

    m_endpoint->onOpen = std::bind(&wss::ChatServer::onConnected, this, std::placeholders::_1);
    m_endpoint->onError = [](WsConnectionPtr conn, const boost::system::error_code &ec) {
      L_WARN_F("Server::Connection::Error", "Connection error[%lu]: %s %s",
               conn->getId(),
               ec.category().name(),
               ec.message().c_str()
      )
    };
    m_endpoint->onClose = std::bind(&wss::ChatServer::onDisconnected,
                                    this,
                                    std::placeholders::_1,
                                    std::placeholders::_2,
                                    std::placeholders::_3);

}
#else
wss::ChatServer::ChatServer(const std::string &host, unsigned short port, const std::string &regexPath) :
    m_useSSL(false),
    m_maxMessageSize(10 * 1024 * 1024),
    m_server(std::make_unique<WsServer>()),
    m_connectionStorage(std::make_unique<wss::ConnectionStorage>()) {
    m_server->config.port = port;
    m_server->config.threadPoolSize = std::thread::hardware_concurrency();
    m_server->config.maxMessageSize = m_maxMessageSize;

    if (host.length() == 15) {
        m_server->config.address = host;
    };

    m_endpoint = &m_server->endpoint[regexPath];
    m_endpoint->onMessage = [this](WsConnectionPtr connectionPtr, WsMessagePtr messagePtr) {
      if (messagePtr->fin_rsv_opcode == FLAG_PONG) {
          onPong(connectionPtr, messagePtr);
          return;
      }
      onMessage(connectionPtr, messagePtr);
    };

    m_endpoint->onOpen = std::bind(&wss::ChatServer::onConnected, this, std::placeholders::_1);
    m_endpoint->onError = [](WsConnectionPtr, const boost::system::error_code &ec) {
        L_WARN_F("Server::Connection::Error", "Connection error: %s %s",
                 ec.category().name(),
                 ec.message().c_str()
        )
    };
    m_endpoint->onClose = std::bind(&wss::ChatServer::onDisconnected,
                                    this,
                                    std::placeholders::_1,
                                    std::placeholders::_2,
                                    std::placeholders::_3);

}
#endif

wss::ChatServer::~ChatServer() {
    stopService();
    joinThreads();
}

void wss::ChatServer::setThreadPoolSize(std::size_t size) {
    m_server->config.threadPoolSize = size;
}
void wss::ChatServer::joinThreads() {
    if (m_workerThread && m_workerThread->joinable()) {
        m_workerThread->join();
    }

    if (m_watchdogThread && m_watchdogThread->joinable()) {
        m_watchdogThread->join();
    }
}
void wss::ChatServer::detachThreads() {
    if (m_workerThread) {
        m_workerThread->detach();
    }
    if (m_watchdogThread) {
        m_watchdogThread->detach();
    }
}
void wss::ChatServer::runService() {
    std::string hostname = "[any:address]";
    if (not m_server->config.address.empty()) {
        hostname = m_server->config.address;
    }
    const char *proto = m_useSSL ? "wss" : "ws";
    L_INFO_F("WebSocket Server", "Started at %s://%s:%d", proto, hostname.c_str(), m_server->config.port);
    m_workerThread = std::make_unique<boost::thread>([this] {
      this->m_server->start();
    });

    if (wss::Settings::get().server.watchdog.enabled) {
        long lifetime = wss::Settings::get().server.watchdog.connectionLifetimeSeconds;
        L_INFO_F("Watchdog", "Started with interval in 1 minute and lifetime=%lu", lifetime);
        m_watchdogThread =
            std::make_unique<boost::thread>(boost::bind(&wss::ChatServer::watchdogWorker, this, lifetime));
    }
}
void wss::ChatServer::stopService() {
    this->m_server->stop();
    if (m_watchdogThread) {
        m_watchdogThread->interrupt();
    }
}

void wss::ChatServer::watchdogWorker(long lifetime) {
    try {
        while (true) {
            boost::this_thread::sleep_for(boost::chrono::minutes(1));
            for (const std::pair<user_id_t, std::unordered_map<conn_id_t, WsConnectionPtr>> &t: m_connectionStorage
                ->get()) {
                const auto &ref = getStat(t.first);
                for (const auto &conn: t.second) {
                    if (!conn.second) {
                        m_connectionStorage->remove(t.first, conn.first);
                        continue;
                    }

                    if (ref->getInactiveTime() >= lifetime) {
                        conn.second->sendClose(STATUS_INACTIVE_CONNECTION,
                                               fmt::format("Inactive more than {0:d} seconds ({1:d})",
                                                           lifetime,
                                                           ref->getInactiveTime()));
                    } else {
                        auto pingStream = std::make_shared<WsMessageStream>();
                        *pingStream << ".";
                        conn.second
                            ->send(std::move(pingStream), [conn, this](const SimpleWeb::ErrorCode &err, std::size_t) {
                              if (err) {
                                  // does not matter, what happens, anyway, this mean connection is bad, broken pipe, eof or something else
                                  m_connectionStorage->remove(conn.second);
                              } else {
                                  // lazy ping pong
                                  m_connectionStorage->markPongWait(conn.second);
                              }
                            }, FLAG_PING);
                    }
                }
            }

            boost::this_thread::sleep_for(boost::chrono::seconds(2));
            std::size_t disconnected = m_connectionStorage->disconnectWithoutPong();
            if (disconnected > 0) {
                L_DEBUG_F("Watchdog", "Disconnected %lu dangling connections", disconnected);
            }
        }
    } catch (const boost::thread_interrupted &) {
        L_INFO("Watchdog", "Stopping...");
    }
}

void wss::ChatServer::onPong(wss::WsConnectionPtr &connection, wss::WsMessagePtr) {
    m_connectionStorage->markPongReceived(connection);
}

void wss::ChatServer::onMessage(WsConnectionPtr &connection, WsMessagePtr message) {
    std::lock_guard<std::recursive_mutex> lock(m_connectionMutex);
    L_DEBUG_F("Chat::Incoming", "On thread: %lu", getThreadName());
    MessagePayload payload;
    const short opcode = message->fin_rsv_opcode;

    if (opcode != FLAG_FRAME_TEXT && opcode != FLAG_FRAME_BINARY) {
        // fragmented frame message
        user_id_t senderId = connection->getId();

        if (opcode == FLAG_FRAGMENT_BEGIN_TEXT || opcode == FLAG_FRAGMENT_BEGIN_BINARY) {
            L_DEBUG_F("Chat::Message", "Fragmented frame begin (flag: 0x%08x)", opcode);
            writeFrameBuffer(senderId, message->string(), true);
            return;
        } else if (opcode == FLAG_FRAGMENT_CONTINUE) {
            writeFrameBuffer(senderId, message->string(), false);
            return;
        } else if (opcode == FLAG_FRAGMENT_END) {
            L_DEBUG("Chat::Message", "Fragmented frame end");
            std::stringstream final;
            final << readFrameBuffer(senderId, true);
            final << message->string();
            std::string buffered = final.str();
            final.str("");
            final.clear();

            /**
             * @Deprecated
             */
            if (buffered.length() > m_maxMessageSize) {
                connection->sendClose(STATUS_MESSAGE_TOO_BIG,
                                      "Message to big. Maximum size: "
                                          + wss::helpers::humanReadableBytes(m_maxMessageSize));
                return;
            }

            payload = MessagePayload(buffered);
        }
    } else {
        // one frame message
        payload = MessagePayload(message->string());
    }

    if (!payload.isValid()) {
        connection->sendClose(STATUS_INVALID_MESSAGE_PAYLOAD, "Invalid payload. " + payload.getError());
        return;
    }

    if (wss::Settings::get().chat.message.enableSendBack) {
        bool isIgnoredType = false;
        for (const auto &ignore: wss::Settings::get().chat.message.ignoreTypesSendBack) {
            if (toolboxpp::strings::equalsIgnoreCase(payload.getType(), ignore)) {
                isIgnoredType = true;
                break;
            }
        }
        if (!isIgnoredType && !payload.isForBot()) {
            sendTo(payload.getSender(), payload);
        }
    }

    send(payload);
}

void wss::ChatServer::onMessageSent(wss::MessagePayload &&payload, std::size_t bytesTransferred, bool hasSent) {
    if (payload.isTypeOfSentStatus()) return;

    getStat(payload.getSender())
        ->addSendMessage()
        .addBytesTransferred(bytesTransferred);

    for (auto &i: payload.getRecipients()) {
        if (hasSent) {
            getStat(i)->addReceivedMessage().addBytesTransferred(bytesTransferred);
        }
    }

    if (m_enableMessageDeliveryStatus && hasSent) {
        wss::MessagePayload status = MessagePayload::createSendStatus(payload);
        send(status);
    }
}

void wss::ChatServer::onConnected(WsConnectionPtr connection) {
    wss::web::Request request;
    request.parseParamsString(connection->queryString);
    request.setHeaders(connection->header);

    if (!m_auth->validateAuth(request)) {
        connection->sendClose(STATUS_UNAUTHORIZED, "Unauthorized");
        return;
    }

    if (request.getParams().empty()) {
        L_WARN_F("Chat::Connect::Error", "Invalid request: %s", connection->queryString.c_str());
        connection->sendClose(STATUS_INVALID_QUERY_PARAMS, "Invalid request");
        return;
    } else if (!request.hasParam("id") || request.getParam("id").empty()) {
        L_WARN("Chat::Connect::Error", "Id required in query parameter: ?id={id}");

        connection->sendClose(STATUS_INVALID_QUERY_PARAMS, "Id required in query parameter: ?id={id}");
        return;
    }

    user_id_t id;
    try {
        id = std::stoul(request.getParam("id"));
    } catch (const std::invalid_argument &e) {
        const std::string errReason = "Passed invalid id: id=" + request.getParam("id") + ". " + e.what();
        L_WARN("Chat::Connect::Error", errReason);
        connection->sendClose(STATUS_INVALID_QUERY_PARAMS, errReason);
        return;
    }

    m_connectionStorage->add(id, connection);
    getStat(id)->addConnection();

    L_DEBUG_F("Chat::Connect", "User %lu connected (%s:%d) on thread %lu",
              id,
              connection->remoteEndpointAddress().c_str(),
              connection->remoteEndpointPort(),
              getThreadName()
    );

    redeliverMessagesTo(id);
}
void wss::ChatServer::onDisconnected(WsConnectionPtr connection, int status, const std::string &reason) {
    if (!m_connectionStorage->exists(connection->getId())) {
        return;
    }

    L_DEBUG_F("Chat::Disconnect", "User %lu (%lu) has disconnected by reason: %s[%d]",
              connection->getId(),
              connection->getUniqueId(),
              reason.c_str(),
              status
    );

    getStat(connection->getId())->addDisconnection();
    m_connectionStorage->remove(connection);
}

bool wss::ChatServer::hasFrameBuffer(wss::user_id_t senderId) {
    return m_frameBuffer.find(senderId) != m_frameBuffer.end();
}

bool wss::ChatServer::writeFrameBuffer(wss::user_id_t senderId, const std::string &input, bool clear) {
    std::lock_guard<std::mutex> fbLock(m_frameBufferMutex);
    if (!hasFrameBuffer(senderId)) {
        m_frameBuffer[senderId] = std::make_shared<std::stringstream>();
    } else if (clear) {
        m_frameBuffer[senderId]->str("");
        m_frameBuffer[senderId]->clear();
    }

    (*m_frameBuffer[senderId]) << input;
    return true;
}
const std::string wss::ChatServer::readFrameBuffer(wss::user_id_t senderId, bool clear) {
    std::lock_guard<std::mutex> fbLock(m_frameBufferMutex);
    if (!hasFrameBuffer(senderId)) {
        return std::string();
    }

    const std::string out = m_frameBuffer[senderId]->str();
    if (clear) {
        m_frameBuffer.erase(senderId);
    }
    return out;
}

int wss::ChatServer::redeliverMessagesTo(const wss::MessagePayload &payload) {
    int cnt = 0;
    for (user_id_t id: payload.getRecipients()) {
        cnt += redeliverMessagesTo(id);
    }

    return cnt;
}
bool wss::ChatServer::hasUndeliveredMessages(user_id_t recipientId) {
    std::lock_guard<std::mutex> locker(m_undeliveredMutex);
    L_DEBUG_F("Chat::Underlivered",
              "Check for undelivered messages for user %lu: %lu",
              recipientId,
              m_undeliveredMessagesMap[recipientId].size());
    return !m_undeliveredMessagesMap[recipientId].empty();
}
wss::MessageQueue &wss::ChatServer::getUndeliveredMessages(user_id_t recipientId) {
    std::lock_guard<std::mutex> locker(m_undeliveredMutex);
    return m_undeliveredMessagesMap[recipientId];
}

std::vector<wss::MessageQueue *>
wss::ChatServer::getUndeliveredMessages(const MessagePayload &payload) {
    std::vector<wss::MessageQueue *> out;
    {
        std::lock_guard<std::mutex> locker(m_undeliveredMutex);
        for (user_id_t id: payload.getRecipients()) {
            out.push_back(&m_undeliveredMessagesMap[id]);
        }
    }

    return out;
}

void wss::ChatServer::enqueueUndeliveredMessage(const wss::MessagePayload &payload) {
    std::unique_lock<std::mutex> uniqueLock(m_undeliveredMutex);
    for (auto recipient: payload.getRecipients()) {
        m_undeliveredMessagesMap[recipient].push(payload);
    }
}
int wss::ChatServer::redeliverMessagesTo(user_id_t recipientId) {
    if (not wss::Settings::get().chat.enableUndeliveredQueue) {
        return 0;
    }

    if (!hasUndeliveredMessages(recipientId)) {
        return 0;
    }

    int cnt = 0;
    auto &queue = getUndeliveredMessages(recipientId);
    L_DEBUG_F("Chat::Undelivered", "Redeliver %lu message(s) to user %lu", queue.size(), recipientId);
    while (!queue.empty()) {
        MessagePayload payload = queue.front();
        queue.pop();
        send(payload);
        cnt++;
    }

    return cnt;
}

void wss::ChatServer::send(const wss::MessagePayload &payload) {
    // if recipient is a BOT, than we don't need to find conneciton, just trigger event notifier ilsteners
    if (payload.isForBot()) {
        callOnMessageListeners(payload);
        L_DEBUG("Chat::Send", "Sending message to bot");
        return;
    }

    callOnMessageListeners(payload);

    for (user_id_t uid: payload.getRecipients()) {
        if (uid == 0L) {
            // just in case, prevent sending bot-only message to nobody
            continue;
        }

        sendTo(uid, payload);
    }
}

void wss::ChatServer::sendTo(user_id_t recipient, const wss::MessagePayload &payload) {
    const std::string payloadString = payload.toJson();
    std::size_t payloadSize = payloadString.length();
    uint8_t fin_rsv_opcode = 129;//@TODO static_cast<uint8_t>(payload.isBinary() ? 130 : 129);

    std::lock_guard<std::recursive_mutex> locker(m_connectionMutex);

    if (!m_connectionStorage->size(recipient)) {
        handleUndeliverable(recipient, payload);
        MessagePayload sent = payload; // copy to move, referenced payload will goes out of scope
        sent.setRecipient(recipient);
        onMessageSent(std::move(sent), payloadSize, false);
        return;
    }

    try {
        const auto &connections = m_connectionStorage->get(recipient);
        int i = 0;
        for (const auto &connection: connections) {
            if (!connection.second) {
                m_connectionStorage->remove(recipient, connection.first);
                continue;
            }

            // DO NOT reuse stream, it will die after first sending
            auto sendStream = std::make_shared<WsMessageStream>();
            *sendStream << payloadString;

            L_DEBUG_F("Chat::Send",
                      "Sending message [thread=%lu] to recipient %lu, connection[%d]=%lu",
                      getThreadName(),
                      recipient,
                      i,
                      connection.second->getUniqueId());

            // connection->send is an asynchronous function
            connection.second
                      ->send(std::move(sendStream),
                             [this, recipient, payload,
                                 cid = connection.first](const SimpleWeb::ErrorCode &errorCode, std::size_t ts) {
                               if (errorCode) {
                                   // See http://www.boost.org/doc/libs/1_55_0/doc/html/boost_asio/reference.html, Error Codes for error code meanings
                                   L_DEBUG_F("Chat::Send::Error", "Unable to send message to %lu. Cause: %s error: %s",
                                             recipient,
                                             errorCode.category().name(),
                                             errorCode.message().c_str()
                                   );
                                   if (errorCode.value() == boost::system::errc::broken_pipe) {
                                       L_DEBUG_F("Chat::Send::Error",
                                                 "Disconnecting Broken connection %lu (%lu)",
                                                 recipient,
                                                 cid);
                                       m_connectionStorage->remove(recipient, cid);
                                   }
                                   handleUndeliverable(recipient, payload);
                               } else {
                                   MessagePayload sent = payload;
                                   sent.setRecipient(recipient);
                                   onMessageSent(std::move(sent), ts, true);
                               }
                             }, fin_rsv_opcode);

            i++;
        }
    } catch (const ConnectionNotFound &) {
        L_DEBUG("Chat::Send", "Connection not found exception. Adding payload to undelivered");
        handleUndeliverable(recipient, payload);
    } catch (const std::exception &e) {
        L_WARN_F("Chat::Send", "Unknown error: %s", e.what());
    } catch (...) {
        L_WARN("Chat::Send", "Unknown error");
    }
}

void wss::ChatServer::handleUndeliverable(wss::user_id_t uid, const wss::MessagePayload &payload) {
    if (!wss::Settings::get().chat.enableUndeliveredQueue) {
        L_DEBUG_F("Chat::Send", "User %lu is unavailable. Skipping message.", uid);
        return;
    }
    MessagePayload inaccessibleUserPayload = payload;
    inaccessibleUserPayload.setRecipient(uid);
    // as messages did not sent to exact user, we add to undelivered queue exact this user in payload recipients
    enqueueUndeliveredMessage(inaccessibleUserPayload);
    L_DEBUG_F("Chat::Send", "User %lu is unavailable. Adding message to queue", uid);
}

std::size_t wss::ChatServer::getThreadName() {
    const std::thread::id id = std::this_thread::get_id();
    static std::size_t nextindex = 0;
    static std::mutex my_mutex;
    static std::map<std::thread::id, std::size_t> ids;
    std::lock_guard<std::mutex> lock(my_mutex);
    if (ids.find(id) == ids.end())
        ids[id] = nextindex++;

    return ids[id];
}
void wss::ChatServer::setMessageSizeLimit(size_t bytes) {
    m_maxMessageSize = bytes;
    m_server->config.maxMessageSize = m_maxMessageSize;
}
void wss::ChatServer::setAuth(const nlohmann::json &config) {
    m_auth = wss::auth::createFromConfig(config);
}

void wss::ChatServer::setEnabledMessageDeliveryStatus(bool enabled) {
    m_enableMessageDeliveryStatus = enabled;
}

void wss::ChatServer::addMessageListener(wss::ChatServer::OnMessageSentListener callback) {
    m_messageListeners.push_back(callback);
}
void wss::ChatServer::addStopListener(wss::ChatServer::OnServerStopListener callback) {
    m_stopListeners.push_back(callback);
}
std::unique_ptr<wss::Statistics> &wss::ChatServer::getStat(wss::user_id_t id) {
    if (m_statistics.find(id) == m_statistics.end()) {
        m_statistics[id] = std::make_unique<wss::Statistics>(id);
    }

    return m_statistics[id];
}

const wss::UserMap<std::unique_ptr<wss::Statistics>> &wss::ChatServer::getStats() {
    return m_statistics;
}
void wss::ChatServer::callOnMessageListeners(wss::MessagePayload payload) {
    for (auto &listener: m_messageListeners) {
        listener(std::move(payload));
    }
}
