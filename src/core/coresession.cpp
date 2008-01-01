/***************************************************************************
 *   Copyright (C) 2005-07 by the Quassel IRC Team                         *
 *   devel@quassel-irc.org                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) version 3.                                           *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "coresession.h"
#include "server.h"

#include "signalproxy.h"
#include "storage.h"

#include "networkinfo.h"
#include "ircuser.h"
#include "ircchannel.h"

#include "util.h"

#include <QtScript>

CoreSession::CoreSession(UserId uid, Storage *_storage, QObject *parent)
  : QObject(parent),
    user(uid),
    _signalProxy(new SignalProxy(SignalProxy::Server, 0, this)),
    storage(_storage),
    scriptEngine(new QScriptEngine(this))
{
  
  QSettings s;
  s.beginGroup(QString("SessionData/%1").arg(user));
  mutex.lock();
  foreach(QString key, s.allKeys()) {
    sessionData[key] = s.value(key);
  }
  mutex.unlock();

  SignalProxy *p = signalProxy();

  p->attachSlot(SIGNAL(requestNetworkStates()), this, SLOT(serverStateRequested()));
  p->attachSlot(SIGNAL(requestConnect(QString)), this, SLOT(connectToNetwork(QString)));
  p->attachSlot(SIGNAL(sendInput(BufferInfo, QString)), this, SLOT(msgFromGui(BufferInfo, QString)));
  p->attachSlot(SIGNAL(requestBacklog(BufferInfo, QVariant, QVariant)), this, SLOT(sendBacklog(BufferInfo, QVariant, QVariant)));
  p->attachSignal(this, SIGNAL(displayMsg(Message)));
  p->attachSignal(this, SIGNAL(displayStatusMsg(QString, QString)));
  p->attachSignal(this, SIGNAL(backlogData(BufferInfo, QVariantList, bool)));
  p->attachSignal(this, SIGNAL(bufferInfoUpdated(BufferInfo)));
  p->attachSignal(storage, SIGNAL(bufferInfoUpdated(BufferInfo)));
  p->attachSignal(this, SIGNAL(sessionDataChanged(const QString &, const QVariant &)), SIGNAL(coreSessionDataChanged(const QString &, const QVariant &)));
  p->attachSlot(SIGNAL(clientSessionDataChanged(const QString &, const QVariant &)), this, SLOT(storeSessionData(const QString &, const QVariant &)));
  /* Autoconnect. (When) do we actually do this?
     --> session restore should be enough!
  QStringList list;
  QVariantMap networks = retrieveSessionData("Networks").toMap();
  foreach(QString net, networks.keys()) {
    if(networks[net].toMap()["AutoConnect"].toBool()) {
      list << net;
    }
  } qDebug() << list;
  if(list.count()) connectToIrc(list);
  */

  initScriptEngine();
}

CoreSession::~CoreSession() {
}

UserId CoreSession::userId() const {
  return user;
}

QVariant CoreSession::state() const {
  QVariantMap res;
  QList<QVariant> conn;
  foreach(Server *server, servers.values()) {
    if(server->isConnected()) {
      QVariantMap m;
      m["Network"] = server->networkName();
      m["State"] = server->state();
      conn << m;
    }
  }
  res["ConnectedServers"] = conn;
  return res;
}

void CoreSession::restoreState(const QVariant &previousState) {
  // Session restore
  QVariantMap state = previousState.toMap();
  if(state.contains("ConnectedServers")) {
    foreach(QVariant v, state["ConnectedServers"].toList()) {
      QVariantMap m = v.toMap();
      QString net = m["Network"].toString();
      if(!net.isEmpty()) connectToNetwork(net, m["State"]);
    }
  }
}


void CoreSession::storeSessionData(const QString &key, const QVariant &data) {
  QSettings s;
  s.beginGroup(QString("SessionData/%1").arg(user));
  mutex.lock();
  sessionData[key] = data;
  s.setValue(key, data);
  mutex.unlock();
  s.endGroup();
  emit sessionDataChanged(key, data);
  emit sessionDataChanged(key);
}

QVariant CoreSession::retrieveSessionData(const QString &key, const QVariant &def) {
  QVariant data;
  mutex.lock();
  if(!sessionData.contains(key)) data = def;
  else data = sessionData[key];
  mutex.unlock();
  return data;
}

// FIXME switch to NetworkIDs
void CoreSession::connectToNetwork(QString network, const QVariant &previousState) {
  uint networkid = getNetworkId(network);
  if(networkid == 0) {
    qWarning() << "unable to connect to Network" << network << "(User:" << userId() << "): unable to determine NetworkId";
    return;
  }
  if(!servers.contains(networkid)) {
    Server *server = new Server(userId(), networkid, network, previousState);
    servers[networkid] = server;
    attachServer(server);
    server->start();
  }
  emit connectToIrc(network);
}

void CoreSession::attachServer(Server *server) {
  connect(this, SIGNAL(connectToIrc(QString)), server, SLOT(connectToIrc(QString)));
  connect(this, SIGNAL(disconnectFromIrc(QString)), server, SLOT(disconnectFromIrc(QString)));
  connect(this, SIGNAL(msgFromGui(uint, QString, QString)), server, SLOT(userInput(uint, QString, QString)));
  
  connect(server, SIGNAL(connected(uint)), this, SLOT(serverConnected(uint)));
  connect(server, SIGNAL(disconnected(uint)), this, SLOT(serverDisconnected(uint)));
  connect(server, SIGNAL(displayMsg(Message::Type, QString, QString, QString, quint8)), this, SLOT(recvMessageFromServer(Message::Type, QString, QString, QString, quint8)));
  connect(server, SIGNAL(displayStatusMsg(QString)), this, SLOT(recvStatusMsgFromServer(QString)));

  // connect serversignals to proxy
  signalProxy()->attachSignal(server, SIGNAL(serverState(QString, QVariantMap)), SIGNAL(networkState(QString, QVariantMap)));
  signalProxy()->attachSignal(server, SIGNAL(connected(uint)), SIGNAL(networkConnected(uint)));
  signalProxy()->attachSignal(server, SIGNAL(disconnected(uint)), SIGNAL(networkDisconnected(uint)));
  // TODO add error handling
}

void CoreSession::serverStateRequested() {
}

void CoreSession::addClient(QIODevice *device) {
  signalProxy()->addPeer(device);
}

SignalProxy *CoreSession::signalProxy() const {
  return _signalProxy;
}

void CoreSession::serverConnected(uint networkid) {
  storage->getBufferInfo(userId(), servers[networkid]->networkName()); // create status buffer
}

void CoreSession::serverDisconnected(uint networkid) {
  Q_ASSERT(servers.contains(networkid));
  servers.take(networkid)->deleteLater();
  Q_ASSERT(!servers.contains(networkid));
}

void CoreSession::msgFromGui(BufferInfo bufid, QString msg) {
  emit msgFromGui(bufid.networkId(), bufid.buffer(), msg);
}

// ALL messages coming pass through these functions before going to the GUI.
// So this is the perfect place for storing the backlog and log stuff.
void CoreSession::recvMessageFromServer(Message::Type type, QString target, QString text, QString sender, quint8 flags) {
  Server *s = qobject_cast<Server*>(this->sender());
  Q_ASSERT(s);
  BufferInfo buf;
  if((flags & Message::PrivMsg) && !(flags & Message::Self)) {
    buf = storage->getBufferInfo(user, s->networkName(), nickFromMask(sender));
  } else {
    buf = storage->getBufferInfo(user, s->networkName(), target);
  }
  Message msg(buf, type, text, sender, flags);
  msg.setMsgId(storage->logMessage(msg));
  Q_ASSERT(msg.msgId());
  emit displayMsg(msg);
}

void CoreSession::recvStatusMsgFromServer(QString msg) {
  Server *s = qobject_cast<Server*>(sender());
  Q_ASSERT(s);
  emit displayStatusMsg(s->networkName(), msg);
}


uint CoreSession::getNetworkId(const QString &net) const {
  return storage->getNetworkId(user, net);
}

QList<BufferInfo> CoreSession::buffers() const {
  return storage->requestBuffers(user);
}


QVariant CoreSession::sessionState() {
  QVariantMap v;

  QVariantList bufs;
  foreach(BufferInfo id, storage->requestBuffers(user))
    bufs.append(QVariant::fromValue(id));
  v["Buffers"] = bufs;

  mutex.lock();
  v["SessionData"] = sessionData;
  mutex.unlock();

  QVariantList networks;
  foreach(uint networkid, servers.keys())
    networks.append(QVariant(networkid));
  v["Networks"] = QVariant(networks);

  // v["Payload"] = QByteArray(100000000, 'a');  // for testing purposes
  return v;
}

void CoreSession::sendBacklog(BufferInfo id, QVariant v1, QVariant v2) {
  QList<QVariant> log;
  QList<Message> msglist;
  if(v1.type() == QVariant::DateTime) {


  } else {
    msglist = storage->requestMsgs(id, v1.toInt(), v2.toInt());
  }

  // Send messages out in smaller packages - we don't want to make the signal data too large!
  for(int i = 0; i < msglist.count(); i++) {
    log.append(QVariant::fromValue(msglist[i]));
    if(log.count() >= 5) {
      emit backlogData(id, log, i >= msglist.count() - 1);
      log.clear();
    }
  }
  if(log.count() > 0) emit backlogData(id, log, true);
}


void CoreSession::initScriptEngine() {
  signalProxy()->attachSlot(SIGNAL(scriptRequest(QString)), this, SLOT(scriptRequest(QString)));
  signalProxy()->attachSignal(this, SIGNAL(scriptResult(QString)));
  
  QScriptValue storage_ = scriptEngine->newQObject(storage);
  scriptEngine->globalObject().setProperty("storage", storage_);
}

void CoreSession::scriptRequest(QString script) {
  emit scriptResult(scriptEngine->evaluate(script).toString());
}
  
