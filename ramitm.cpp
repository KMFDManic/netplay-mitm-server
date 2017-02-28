/* (C) Copyright 2017 - Brad Parker */
/* License: GPL */

#include "ramitm.h"
#include <stdio.h>

#ifdef Q_OS_UNIX
#include <signal.h>
#endif

#include <arpa/inet.h>

static QString getPeerIPv4(QHostAddress addr) {
  struct in_addr ip_addr;
  ip_addr.s_addr = addr.toIPv4Address();

  return QString(inet_ntoa(ip_addr));
}

#define QC_STR(x) x.toStdString().c_str()
#define HOST(x) getPeerIPv4(x->peerAddress())
#define PORT(x) x->peerPort()
#define CLIENT_LOG(x, y) printf("%s:%d %s\n", QC_STR(HOST(x)), PORT(x), y)
#define CLIENT_LOGF(x, fmt, ...) printf("%s:%d ", QC_STR(HOST(x)), PORT(x)); printf(fmt, __VA_ARGS__)

size_t strlcpy(char *dest, const char *source, size_t size)
{
  size_t src_size = 0;
  size_t n = size;

  if(n)
    while(--n && (*dest++ = *source++)) src_size++;

  if(!n) {
    if(size) *dest = '\0';
    while(*source++) src_size++;
  }

  return src_size;
}

RAMITM::RAMITM(QObject *parent) :
  QObject(parent)
  ,m_sock(new QTcpServer(this))
  ,m_header()
  ,m_info()
  ,m_info_set(false)
  ,m_first_sync_sent(false)
  ,m_sockets()
  ,m_frameNumber(0)
{
  memset(&m_info, 0, sizeof(m_info));

#ifdef Q_OS_UNIX
  struct sigaction sigint, sigterm;

  sigint.sa_handler = RAMITM::handleSIGINT;
  sigemptyset(&sigint.sa_mask);
  sigint.sa_flags = 0;
  sigint.sa_flags |= SA_RESTART;

  sigaction(SIGINT, &sigint, 0);

  sigterm.sa_handler = RAMITM::handleSIGTERM;
  sigemptyset(&sigterm.sa_mask);
  sigterm.sa_flags = 0;
  sigterm.sa_flags |= SA_RESTART;

  sigaction(SIGTERM, &sigterm, 0);
#endif

  connect(m_sock, SIGNAL(acceptError(QAbstractSocket::SocketError)), this, SLOT(acceptError(QAbstractSocket::SocketError)));
  connect(m_sock, SIGNAL(newConnection()), this, SLOT(newConnection()));
}

void RAMITM::start() {
  if(!m_sock->listen(QHostAddress::Any, 5905)) {
    printf("could not bind to port\n");
    QCoreApplication::quit();
    return;
  }

  printf("bound to port %d\n", m_sock->serverPort());
}

void RAMITM::handleSIGINT(int) {
  printf("quitting...\n");
  QCoreApplication::quit();
}

void RAMITM::handleSIGTERM(int) {
  printf("quitting...\n");
  QCoreApplication::quit();
}

void RAMITM::acceptError(QAbstractSocket::SocketError socketError) {
  printf("got accept() error code %d\n", socketError);
}

void RAMITM::newConnection() {
  QTcpSocket *sock = m_sock->nextPendingConnection();

  if(!sock)
    return;

  m_sockets.append(sock);

  CLIENT_LOG(sock, "got new connection");

  connect(sock, SIGNAL(readyRead()), this, SLOT(readyRead()));
  connect(sock, SIGNAL(disconnected()), this, SLOT(disconnected()));
  connect(sock, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(error(QAbstractSocket::SocketError)));
}

void RAMITM::readyRead() {
  QTcpSocket *sock = static_cast<QTcpSocket*>(sender());

  if(!sock) {
    printf("ERROR: no socket in readyRead\n");
    return;
  }

  ClientState state = static_cast<ClientState>(sock->property("state").toUInt());
  uint32_t cmd[2] = {0};

  //printf("readyRead: %lli bytes available, state is %d for host %s\n", sock->bytesAvailable(), state, QC_STR(sock->peerAddress().toString()));

  /*int bytes = sock->bytesAvailable();

  for(int i = 0; i < bytes; i++) {
    char a;
    sock->read(&a, 1);
    printf(" %08X", a);
  }

  printf("\n");*/

  // check for end of mandatory state handling before accepting any new commands
  if(state >= STATE_NONE) {
    if(sock->bytesAvailable() < 8) {
      // wait for more data
      //printf("not enough data for a command yet, keep waiting\n");
      return;
    }

    // after the header we switch to a command-based format
    uint32_t newcmd[2];

    qint64 readBytes = sock->read((char*)newcmd, 8);

    if(readBytes != 8) {
      CLIENT_LOG(sock, "invalid data received, aborting connection");
      sock->deleteLater();
      return;
    }

    cmd[0] = ntohl(newcmd[0]);
    cmd[1] = ntohl(newcmd[1]);

    if(sock->bytesAvailable() < (qint64)cmd[1]) {
      CLIENT_LOGF(sock, "WARNING: not enough data, we have %lli bytes available but the command payload size is %u\n", sock->bytesAvailable(), cmd[1]);
    }

    if(state == STATE_NONE) {
      if(cmd[0] == CMD_ACK) {
        CLIENT_LOG(sock, "ACK");
        // acknowledge, wait for next command
      }

      if(cmd[0] == CMD_NACK || cmd[0] == CMD_DISCONNECT) {
        CLIENT_LOG(sock, "client didn't like our data or requested a disconnect, aborting connection");
        sock->deleteLater();
        return;
      }

      switch(cmd[0]) {
        case CMD_NICK:
          state = STATE_RECV_NICKNAME;
          break;
        case CMD_INFO:
          state = STATE_RECV_INFO;
          break;
        case CMD_PLAY:
          state = STATE_RECV_PLAY;
          break;
        case CMD_INPUT:
          state = STATE_RECV_INPUT;
          break;
        default:
          break;
      }

      if(cmd[0] != CMD_INPUT) {
        CLIENT_LOGF(sock, "got command %08X with payload size %u\n", cmd[0], cmd[1]);
      }
    }
  }

  switch(state) {
    case STATE_HEADER:
    {
      if(m_sockets.indexOf(sock) == 0) {
        if(sock->bytesAvailable() < 16) {
          // not enough data available yet, keep waiting
          CLIENT_LOGF(sock, "header: not enough data available, only %lli bytes out of 16\n", sock->bytesAvailable());
          return;
        }else{
          char header[HEADER_LEN];

          qint64 readBytes = sock->read(header, HEADER_LEN);

          if(readBytes != HEADER_LEN) {
            CLIENT_LOG(sock, "no header received, aborting connection");
            sock->deleteLater();
            return;
          }

          CLIENT_LOGF(sock, "header: got %lli bytes\n", readBytes);

          // store the first client's header to use for all others
          memcpy(m_header, header, HEADER_LEN);
        }
      }

      // send header to client, we already have the first connection's header at this point
      qint64 wroteBytes = sock->write(m_header, HEADER_LEN);

      if(wroteBytes != HEADER_LEN) {
        CLIENT_LOG(sock, "could not send header to client, aborting connection");
        sock->deleteLater();
        return;
      }else{
        CLIENT_LOG(sock, "sent header to client");
      }

      if(m_sockets.count() > 1) {
        // read connection header back and verify it is the same as the first client
        char header[HEADER_LEN];

        qint64 readBytes = sock->read(header, HEADER_LEN);

        if(readBytes != HEADER_LEN) {
          CLIENT_LOG(sock, "no header received, aborting connection");
          sock->deleteLater();
          return;
        }

        CLIENT_LOGF(sock, "SVR header: %08X %08X %08X %08X\n", m_header[0], m_header[1], m_header[2], m_header[3]);
        CLIENT_LOGF(sock, "CLT header: %08X %08X %08X %08X\n", header[0], header[1], header[2], header[3]);

        if(memcmp(header, m_header, HEADER_LEN)) {
          // header did not match the first connection
          CLIENT_LOG(sock, "header did not match the first connection, aborting");
          sock->deleteLater();
          return;
        }else{
          CLIENT_LOG(sock, "header matches");
        }
      }

      sock->setProperty("state", STATE_SEND_NICKNAME);

      break;
    }
    case STATE_SEND_NICKNAME:
    {
      struct nick_buf_s nick;

      memset(&nick, 0, sizeof(nick));

      nick.cmd[0] = htonl(CMD_NICK);
      nick.cmd[1] = htonl(sizeof(nick.nick));

      strlcpy(nick.nick, "NICK", 5);

      sock->write((const char *)&nick, sizeof(nick));

      sock->setProperty("state", STATE_NONE);

      CLIENT_LOG(sock, "sent nick to host");

      break;
    }
    case STATE_RECV_NICKNAME:
    {
      struct nick_buf_s nick;

      if(sock->bytesAvailable() < (qint64)sizeof(nick.nick)) {
        // not enough data available yet, keep waiting
        CLIENT_LOGF(sock, "recv nick: not enough data available, only %lli bytes out of %li\n", sock->bytesAvailable(), sizeof(nick));
        return;
      }

      if(cmd[1] != sizeof(nick.nick)) {
        CLIENT_LOGF(sock, "nickname size is wrong (%08X), aborting\n", cmd[1]);
        sock->deleteLater();
        return;
      }

      qint64 readBytes = sock->read(nick.nick, sizeof(nick.nick));

      if(readBytes != sizeof(nick.nick)) {
        CLIENT_LOGF(sock, "could not read nickname from client. got %lli bytes when expecting %li, aborting\n", readBytes, sizeof(nick.nick));
        sock->deleteLater();
        return;
      }

      CLIENT_LOGF(sock, "nick: got %lli bytes\n", readBytes);

      CLIENT_LOGF(sock, "nick is %s\n", nick.nick);

      sock->setProperty("state", STATE_SEND_INFO);

      // force sending of INFO without waiting for new data
      readyRead();

      break;
    }
    case STATE_SEND_INFO:
    {
      size_t info_payload_size = m_info_set ? (sizeof(m_info) - sizeof(m_info.cmd)) : 0;

      m_info.cmd[0] = htonl(CMD_INFO);

      // remove the length of the cmd member from the payload size
      m_info.cmd[1] = htonl(info_payload_size);

      sock->write((const char *)&m_info, sizeof(m_info.cmd) + info_payload_size);

      CLIENT_LOG(sock, "sent info to host");

      if(m_info_set) {
        CLIENT_LOG(sock, "next state is send sync");
        sock->setProperty("state", STATE_SEND_SYNC);
        readyRead();
      }else{
        CLIENT_LOG(sock, "next state is none");
        sock->setProperty("state", STATE_NONE);
      }

      break;
    }
    case STATE_RECV_INFO:
    {
      struct info_buf_s info;
      size_t info_payload_size = sizeof(info) - sizeof(info.cmd);

      info.cmd[0] = htonl(cmd[0]);
      info.cmd[1] = htonl(cmd[1]);

      if(sock->bytesAvailable() < (qint64)info_payload_size) {
        // not enough data available yet, keep waiting
        CLIENT_LOGF(sock, "recv info: not enough data available, only %lli bytes out of %li\n", sock->bytesAvailable(), info_payload_size);
        return;
      }

      if(cmd[1] != info_payload_size) {
        CLIENT_LOGF(sock, "info size is wrong (%08X), aborting\n", cmd[1]);
        sock->deleteLater();
        return;
      }

      qint64 readBytes = sock->read(info.core_name, info_payload_size);

      if(readBytes != (qint64)info_payload_size) {
        CLIENT_LOGF(sock, "could not read info from client. got %lli bytes when expecting %li, aborting\n", readBytes, info_payload_size);
        sock->deleteLater();
        return;
      }

      if(m_sockets.indexOf(sock) == 0) {
        if(m_first_sync_sent) {
          // the first client is just echoing back the info we already have, ignore it
          sock->setProperty("state", STATE_NONE);
          break;
        }else{
          // save the first INFO to echo back to all other clients
          memcpy(&m_info, &info, sizeof(info));
          m_info_set = true;
        }
      }else if(m_sockets.indexOf(sock) > 0) {
        // make sure other clients have the same INFO
        if(!memcmp(&m_info, &info, sizeof(info))) {
          // info matches
          CLIENT_LOG(sock, "info matches");
          sock->setProperty("state", STATE_NONE);
          break;
        }else{
          // no match, disconnect client
          CLIENT_LOG(sock, "info from client did not match, aborting connection");
          sock->deleteLater();
          return;
        }
      }

      CLIENT_LOGF(sock, "info: got %lli bytes from %s\n", readBytes, QC_STR(sock->peerAddress().toString()));

      CLIENT_LOGF(sock, "info: core name is %s\n", info.core_name);
      CLIENT_LOGF(sock, "info: core version is %s\n", info.core_version);
      CLIENT_LOGF(sock, "info: content crc is %08X\n", ntohl(info.content_crc));

      sock->setProperty("state", STATE_SEND_INFO);

      // force sending of INFO without waiting for new data
      readyRead();

      break;
    }
    case STATE_SEND_SYNC:
    {
      sync_buf_s sync;
      size_t sync_payload_size = sizeof(sync) - 2 * sizeof(uint32_t);

      memset(&sync, 0, sizeof(sync));

      sync.cmd[0] = htonl(CMD_SYNC);

      // remove the length of the cmd member from the payload size
      sync.cmd[1] = htonl(sync_payload_size);

      for(int i = 0; i < m_sockets.count(); ++i) {
        QTcpSocket *player = m_sockets.at(i);

        if(!player)
          continue;

        if(!player->property("sync_sent").toBool()) {
          // don't count other players that haven't finished their handshake yet
          continue;
        }

        if(player == sock) {
          // we don't count ourselves
          continue;
        }

        sync.players |= 1U << i;
      }

      sync.players = htonl(sync.players);
      sync.frame_num = htonl(m_frameNumber);
      sync.devices[0] = htonl(1);
      sync.devices[1] = htonl(1);

      sock->write((const char *)&sync, sizeof(sync));

      if(m_sockets.indexOf(sock) == 0)
        m_first_sync_sent = true;

      sock->setProperty("state", STATE_NONE);
      sock->setProperty("sync_sent", true);

      CLIENT_LOG(sock, "sent sync to host");

      break;
    }
    case STATE_RECV_PLAY:
    {
      if(cmd[1] > 0)
      {
        // not using the payload right now
        sock->read(cmd[1]);
      }

      mode_buf_s mode;

      memset(&mode, 0, sizeof(mode));

      mode.cmd[0] = htonl(CMD_MODE);
      mode.cmd[1] = htonl(sizeof(mode) - sizeof(mode.cmd));

      mode.frame_num = htonl(m_frameNumber);
      mode.player_num = htons(m_sockets.indexOf(sock));

      foreach(QTcpSocket *player, m_sockets) {
        if(!player->property("sync_sent").toBool()) {
          // don't send MODE to other players that haven't finished their handshake yet
          continue;
        }

        if(player == sock) {
          mode.target = htons(3); // bit0 == is MODE being sent to the affected player, bit1 == is the user now playing or spectating
        }else{
          mode.target = htons(2);
        }

        player->write((const char *)&mode, sizeof(mode));
      }

      CLIENT_LOG(sock, "received PLAY and sent MODE to all users");

      sock->setProperty("state", STATE_NONE);

      break;
    }
    case STATE_RECV_INPUT:
    {
      input_buf_s input;
      size_t input_payload_size = sizeof(input) - sizeof(input.cmd);

      if(sock->bytesAvailable() < (qint64)input_payload_size) {
        // not enough data available yet, keep waiting
        CLIENT_LOGF(sock, "recv input: not enough data available, only %lli bytes out of %li\n", sock->bytesAvailable(), input_payload_size);
        return;
      }

      if(cmd[1] != input_payload_size) {
        CLIENT_LOGF(sock, "input size is wrong (%08X), aborting\n", cmd[1]);
        sock->deleteLater();
        return;
      }

      qint64 readBytes = sock->read((char*)&input.frame_num, input_payload_size);

      if(readBytes != (qint64)input_payload_size) {
        CLIENT_LOGF(sock, "could not read input from client. got %lli bytes when expecting %li, aborting\n", readBytes, input_payload_size);
        sock->deleteLater();
        return;
      }

      if(m_sockets.indexOf(sock) == 0) {
        // this is the first (master) connection
        // server follows the first connection's frame number
        m_frameNumber = ntohl(input.frame_num);
      }

      // server is transparent, so we send NOINPUT back to all the clients to tell them we aren't sending it any input ourselves
      // (and that the server is done with this frame completely!)
      noinput_buf_s noinput;
      noinput.cmd[0] = htonl(CMD_NOINPUT);
      noinput.cmd[1] = htonl(sizeof(noinput) - sizeof(noinput.cmd));
      noinput.frame_num = htonl(m_frameNumber);

      // forward this INPUT to everyone else, and send NOINPUT to everyone
      foreach(QTcpSocket *player, m_sockets) {
        if(player->property("sync_sent").toBool()) {
          if(player != sock) {
            // send this INPUT to all other handshook players
            player->write((const char *)&input, sizeof(input));
          }

          if(m_sockets.indexOf(sock) == 0) {
            // send NOINPUT to everyone, but only when getting an INPUT from the master client, as we are keeping our frames in sync with it
            player->write((const char *)&noinput, sizeof(noinput));
          }
        }
      }

      /*if(m_frameNumber % 100 == 0) {
        CLIENT_LOGF(sock, "received INPUT and sent NOINPUT %u\n", m_frameNumber);
      }*/

      sock->setProperty("state", STATE_NONE);

      if(m_sockets.indexOf(sock) == 0)
      {
        // Increment server frame number ahead of master client sending a new INPUT with the same frame number.
        // This allows sending MODE to new players as the first event of the next frame, before the master's INPUT.
        ++m_frameNumber;
      }

      break;
    }
    default:
      // ignore unknown command
      CLIENT_LOGF(sock, "ignoring unknown command %08X with size %u\n", cmd[0], cmd[1]);

      if(cmd[1] > 0)
        sock->read(cmd[1]);

      break;
  }

  // if we didn't use all the data we got, keep reading
  if(sock->bytesAvailable() > 0)
  {
    //CLIENT_LOGF(sock, "still %lli bytes left, queueing readyRead\n", sock->bytesAvailable());
    readyRead();
  }
}

void RAMITM::disconnected() {
  QTcpSocket *sock = static_cast<QTcpSocket*>(sender());

  if(!sock)
    return;

  m_sockets.removeOne(sock);

  CLIENT_LOG(sock, "client disconnected");

  sock->deleteLater();
}

void RAMITM::error(QAbstractSocket::SocketError socketError) {
  // NOTE: only attempt a reconnect here if using a 0-timer

  QTcpSocket *sock = static_cast<QTcpSocket*>(sender());

  if(!sock)
    return;

  CLIENT_LOGF(sock, "client got socket error %d\n", socketError);

  // disconnected() will be emitted (because of the error, or by this destructor?)
  sock->deleteLater();
}
