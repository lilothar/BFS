/**********************************************************************
Copyright (C) <2014>  <Behrooz Shafiee Sarjaz>
This program comes with ABSOLUTELY NO WARRANTY;

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
**********************************************************************/

#include "BFSTcpServer.h"
#include "BFSTcpServiceHandler.h"
#include "SettingManager.h"
#include <Poco/Net/ParallelSocketAcceptor.h>
#include <Poco/Net/SocketAcceptor.h>
#include "LoggerInclude.h"
#include "ZooHandler.h"
#include <string.h> /* for strncpy */
#include <iostream>
#include <string.h>
#include "Timer.h"
#include "MemoryController.h"

using namespace Poco;
using namespace Poco::Net;
using namespace std;
using namespace BFSTCPNetworkTypes;


SocketReactor *BFSTcpServer::reactor = nullptr;
thread *BFSTcpServer::thread = nullptr;
uint32_t BFSTcpServer::port = 0;
string BFSTcpServer::ip = "";
string BFSTcpServer::iface;
atomic<bool> BFSTcpServer::initialized(false);
atomic<bool> BFSTcpServer::initSuccess(false);
//Transfer stuff
BFS::Queue<TransferTask*> BFSTcpServer::transferQueue;
std::thread *BFSTcpServer::transferThread = nullptr;
atomic<bool> BFSTcpServer::isRunning(true);

BFSTcpServer::BFSTcpServer() {}

BFSTcpServer::~BFSTcpServer() {}

void BFSTcpServer::run() {
  try{
    initialize();
    ServerSocket serverSocket(port);
    reactor = new SocketReactor();
    //SocketAcceptor<BFSTcpServiceHandler> acceptor(serverSocket, *reactor);
    ParallelSocketAcceptor<BFSTcpServiceHandler,SocketReactor> acceptor(serverSocket, *reactor);

    //Start Transfer Thread
    transferThread = new std::thread(BFSTcpServer::transferLoop);

    initSuccess.store(true);
    initialized.store(true);

    //Start Reactor
    reactor->run();
  }catch(Exception&e){
    LOG(ERROR)<<"ERROR in initializing BFSTCPServer:"<<e.message();
    initialized.store(true);
    initSuccess.store(false);
    return;
  }
}

bool BFSTcpServer::start() {
  //Start Server Socket
  thread = new std::thread(run);

  //Wait for initialization
  while(!initialized)
    usleep(10);

  return initSuccess;
}

void BFSTcpServer::stop() {
  LOG(INFO)<<"STOPPING REACTOR!";
  isRunning.store(false);
  //Stop Transfer Thread
  transferQueue.stop();
  if(transferThread){
    transferThread->join();
    delete transferThread;
  }

  //Stop reactor
  if(reactor)
    reactor->stop();
  if(thread)
    thread->join();
  usleep(100);
  if(reactor){
    delete reactor;
  }
  if(thread){
    delete thread;
  }
  reactor = nullptr;
  thread = nullptr;
  reactor = nullptr;
  transferThread = nullptr;
}

int64_t BFSTcpServer::readRemoteFile(void* _dstBuffer, uint64_t _size,
    size_t _offset, const std::string& _remoteFile,
    const std::string& _ip,uint _port) {

  if(_remoteFile.length() >= 1024){
    LOG(ERROR) <<"Filename too long:"<<_remoteFile;
    return -1;
  }

  StreamSocket socket;
  try {
    SocketAddress addres(_ip,_port);
    socket.connect(addres);
    socket.setKeepAlive(false);
  }catch(Exception &e){
    LOG(ERROR)<<"Error in createing socket to:"<<_ip<<":"<<_port<<" why?"<<e.message();
    return -2;
  }

  //Create a request
  ReadReqPacket readReqPacket;
  strncpy(readReqPacket.fileName,_remoteFile.c_str(),_remoteFile.length());
  readReqPacket.fileName[_remoteFile.length()]= '\0';
  readReqPacket.offset = htobe64(_offset);
  readReqPacket.size = htobe64(_size);
  readReqPacket.opCode = htonl((uint32_t)BFS_REMOTE_OPERATION::READ);

  //Send Packet
  try {
    socket.sendBytes((void*)&readReqPacket,sizeof(ReadReqPacket));
  }catch(Exception &e){
    LOG(ERROR)<<"Error in sending read request"<<_remoteFile<<": "<<e.message();
    try{
      socket.close();
    }catch(...){}
    return -2;
  }
  //Now read Status
  ReadResPacket resPacket;
  try {
    socket.receiveBytes((void*)&resPacket,sizeof(ReadResPacket));
  }catch(Exception &e){
    LOG(ERROR)<<"Error in receiving read status packet"<<_remoteFile<<": "<<e.message();
    try{
      socket.close();
    }catch(...){}
    return -2;
  }
  resPacket.statusCode = be64toh(resPacket.statusCode);
  resPacket.readSize = be64toh(resPacket.readSize);
  if(resPacket.statusCode == 200 && resPacket.readSize > 0){//Successful
    try {
      int count = 0;
      uint64_t total = 0;
      uint64_t left = resPacket.readSize;

      /*BFS::Timer t;
      t.begin();*/

      do {
        count = socket.receiveBytes((void*)((char*)_dstBuffer+total),left);
        total += count;
        left -= count;
      } while(left > 0 && count);


      /*t.end();
      LOG(ERROR)<<" Read:"<<total<<" bytes in:"<<t.elapsedMillis()<<" millis";*/

      try {
        socket.close();
      } catch(...){}
      if(total != resPacket.readSize)
        return -4;
      else
        return total;
    }catch(Exception &e){
      LOG(ERROR)<<"Error in receiving read status packet"<<_remoteFile<<": "<<e.message();
      try{
        socket.close();
      }catch(...){}
      return -2;
    }
  } else if(resPacket.statusCode == 200 && resPacket.readSize == 0){//EOF
    try {
      socket.close();
    } catch(...){}
    return 0;
  }
  //Unsuccessful
  try {
    socket.close();
  } catch(...){}
  return -resPacket.statusCode;
}

int64_t BFSTcpServer::writeRemoteFile(const void* _srcBuffer, uint64_t _size,
    size_t _offset, const std::string& _remoteFile,
    const std::string &_ip,uint _port) {
  if(_remoteFile.length() >= 1024){
    LOG(ERROR) <<"Filename too long:"<<_remoteFile;
    return -1;
  }

  StreamSocket socket;
  try {
    SocketAddress addres(_ip,_port);
    socket.connect(addres);
    socket.setKeepAlive(false);
  }catch(Exception &e){
    LOG(ERROR)<<"Error in creating socket to:"<<_ip<<":"<<_port;
    return -2;
  }

  //Create a request
  WriteReqPacket writeReqPacket;
  strncpy(writeReqPacket.fileName,_remoteFile.c_str(),_remoteFile.length());
  writeReqPacket.fileName[_remoteFile.length()]= '\0';
  writeReqPacket.offset = htobe64(_offset);
  writeReqPacket.size = htobe64(_size);
  writeReqPacket.opCode = htonl((uint32_t)BFS_REMOTE_OPERATION::WRITE);

  //Send Packet
  try {
    socket.sendBytes((void*)&writeReqPacket,sizeof(WriteReqPacket));
  }catch(Exception &e){
    LOG(ERROR)<<"Error in sending write request"<<_remoteFile<<": "<<e.message();
    try{
      socket.close();
    }catch(...){}
    return -2;
  }

  //Now send real write data
  try {
    uint64_t left = _size;
    do{
      int sent = socket.sendBytes((char*)_srcBuffer+(_size-left),left);
      left -= sent;
      if(sent <= 0){//Error
        LOG(ERROR)<<"ERROR in sending write data, asked to send:"<<left<<" but sent:"<<sent;
        break;
      }
    }while(left);
  }catch(Exception &e){
    try {
      socket.close();
    } catch(...){}
    LOG(ERROR)<<"Error in sending write data:"<<_remoteFile<<":"<<e.message();
    //Might be because I am writing to the wrong node! so let's update global view
    BFS::ZooHandler::getInstance().requestUpdateGlobalView();
    return -2;
  }

  //Now read writeStatus
  WriteResPacket resPacket;
  try {
    socket.receiveBytes((void*)&resPacket,sizeof(WriteResPacket));
  }catch(Exception &e){
    LOG(ERROR)<<"Error in receiving write status packet"<<_remoteFile<<": "<<e.message();
    try {
      socket.close();
    } catch(...){}
    return -2;
  }

  try {
    socket.close();
  } catch(...){}

  resPacket.statusCode = be64toh(resPacket.statusCode);
  if(resPacket.statusCode == 200)
    return _size;
  else
    return resPacket.statusCode;//Unsuccessful(or transfer)
}

int64_t BFSTcpServer::attribRemoteFile(struct packed_stat_info* attBuff,
    const std::string& _remoteFile, const std::string& _ip, uint _port) {
  if(_remoteFile.length() >= 1024){
    LOG(ERROR) <<"Filename too long:"<<_remoteFile;
    return -1;
  }

  StreamSocket socket;
  try {
    SocketAddress addres(_ip,_port);
    socket.connect(addres);
    socket.setKeepAlive(false);
    socket.setNoDelay(true);
    //socket.setReceiveTimeout(Timespan(5,0));
  }catch(Exception &e){
    LOG(ERROR)<<"Error in creating socket to:"<<_ip<<":"<<_port;
    return -2;
  }

  //Create a request
  AttribReqPacket attribReqPacket;
  strncpy(attribReqPacket.fileName,_remoteFile.c_str(),_remoteFile.length());
  attribReqPacket.fileName[_remoteFile.length()]= '\0';
  attribReqPacket.opCode = htonl((uint32_t)BFS_REMOTE_OPERATION::ATTRIB);

  //Send Packet
  try {
    socket.sendBytes((void*)&attribReqPacket,sizeof(AttribReqPacket));
  }catch(Exception &e){
    LOG(ERROR)<<"Error in sending Attrib request"<<_remoteFile<<": "<<e.message();
    try{
      socket.close();
    }catch(...){}
    return -2;
  }
  //Now Attrib Status
  AttribResPacket resPacket;
  try {
    socket.receiveBytes((void*)&resPacket,sizeof(AttribResPacket));
  }catch(Exception &e){
    LOG(ERROR)<<"Error in receiving Attrib status packet"<<_remoteFile<<": "<<e.message();
    try{
      socket.close();
    }catch(...){}
    return -2;
  }
  resPacket.statusCode = be64toh(resPacket.statusCode);
  resPacket.attribSize = be64toh(resPacket.attribSize);
  if(resPacket.statusCode == 200 && resPacket.attribSize > 0){//Successful
    try {
      int count = 0;
      uint64_t total = 0;
      uint64_t left = resPacket.attribSize;
      do {
        count = socket.receiveBytes((void*)((char*)attBuff+total),left);
        total += count;
        left -= count;
      } while(left > 0 && count);

      try {
        socket.close();
      } catch(...){}
      if(total != resPacket.attribSize)
        return -4;
      else
        return 200;
    }catch(Exception &e){
      LOG(ERROR)<<"Error in receiving attrib status packet"<<_remoteFile<<": "<<e.message();
      try{
        socket.close();
      }catch(Exception &e){
        LOG(ERROR)<<"Error in shutting down connection of remote attrib: "<<_remoteFile<<": "<<e.message();
      }
      return -2;
    }
  }
  try {
    socket.close();
  } catch(...){}
  //Unsuccessful
  return resPacket.statusCode;
}

int64_t BFSTcpServer::deleteRemoteFile(const std::string& remoteFile,
    const std::string& _ip, uint _port) {
  if(remoteFile.length() > 1024){
      LOG(ERROR)<<"Filename too long!";
    return false;
  }

  StreamSocket socket;
  try {
   SocketAddress addres(_ip,_port);
   socket.connect(addres);
   socket.setKeepAlive(false);
   socket.setNoDelay(true);
  }catch(Exception &e){
   LOG(ERROR)<<"Error in creating socket to:"<<_ip<<":"<<_port;
   return -2;
  }

  //Create a request
  DeleteReqPacket deleteReqPacket;
  strncpy(deleteReqPacket.fileName,remoteFile.c_str(),remoteFile.length());
  deleteReqPacket.fileName[remoteFile.length()]= '\0';
  deleteReqPacket.opCode = htonl((uint32_t)BFS_REMOTE_OPERATION::DELETE);

  //Send Packet
  try {
   socket.sendBytes((void*)&deleteReqPacket,sizeof(DeleteReqPacket));
  }catch(Exception &e){
   LOG(ERROR)<<"Error in sending Delete request"<<remoteFile<<": "<<e.message();
   try {
     socket.close();
   }catch(...){}
   return -2;
  }

  //Now Delete Status
  DeleteResPacket resPacket;
  try {
    socket.receiveBytes((void*)&resPacket,sizeof(DeleteResPacket));
  }catch(Exception &e){
    LOG(ERROR)<<"Error in receiving Delete status packet"<<remoteFile<<": "<<e.message();
    try{
      socket.close();
    }catch(...){}
    return -2;
  }

  try {
    socket.close();
  } catch(...){}
  resPacket.statusCode = be64toh(resPacket.statusCode);
  return resPacket.statusCode;
}

int64_t BFSTcpServer::flushRemoteFile(const std::string& remoteFile,
    const std::string& _ip, uint _port) {
  if(remoteFile.length() > 1024){
      LOG(ERROR)<<"Filename too long!";
    return false;
  }

  StreamSocket socket;
  try {
   SocketAddress addres(_ip,_port);
   socket.connect(addres);
   socket.setKeepAlive(false);
   socket.setNoDelay(true);
  }catch(Exception &e){
   LOG(ERROR)<<"Error in creating socket to:"<<_ip<<":"<<_port;
   return -2;
  }

  //Create a request
  FlushReqPacket flushReqPacket;
  strncpy(flushReqPacket.fileName,remoteFile.c_str(),remoteFile.length());
  flushReqPacket.fileName[remoteFile.length()]= '\0';
  flushReqPacket.opCode = htonl((uint32_t)BFS_REMOTE_OPERATION::FLUSH);

  //Send Packet
  try {
   socket.sendBytes((void*)&flushReqPacket,sizeof(FlushReqPacket));
  }catch(Exception &e){
   LOG(ERROR)<<"Error in sending flush request"<<remoteFile<<": "<<e.message();
   try {
     socket.close();
   }catch(...){}
   return -2;
  }

  //Now Flush Status
  FlushResPacket resPacket;
  try {
    socket.receiveBytes((void*)&resPacket,sizeof(FlushResPacket));
  }catch(Exception &e){
    LOG(ERROR)<<"Error in receiving Flush status packet"<<remoteFile<<": "<<e.message();
    try{
      socket.close();
    }catch(...){}
    return -2;
  }

  try {
    socket.close();
  } catch(...){}
  resPacket.statusCode = be64toh(resPacket.statusCode);
  return resPacket.statusCode;
}

int64_t BFSTcpServer::truncateRemoteFile(const std::string& remoteFile,
    size_t _newSize,const std::string& _ip, uint _port) {
  if(remoteFile.length() > 1024){
      LOG(ERROR)<<"Filename too long!";
    return false;
  }

  StreamSocket socket;
  try {
   SocketAddress addres(_ip,_port);
   socket.connect(addres);
   socket.setKeepAlive(false);
   socket.setNoDelay(true);
  }catch(Exception &e){
   LOG(ERROR)<<"Error in creating socket to:"<<_ip<<":"<<_port;
   return -2;
  }

  //Create a request
  TruncReqPacket truncReqPacket;
  strncpy(truncReqPacket.fileName,remoteFile.c_str(),remoteFile.length());
  truncReqPacket.fileName[remoteFile.length()]= '\0';
  truncReqPacket.opCode = htonl((uint32_t)BFS_REMOTE_OPERATION::TRUNCATE);
  truncReqPacket.size = htobe64(_newSize);

  //Send Packet
  try {
   socket.sendBytes((void*)&truncReqPacket,sizeof(TruncReqPacket));
  }catch(Exception &e){
   LOG(ERROR)<<"Error in sending Truncate request"<<remoteFile<<": "<<e.message();
   try {
     socket.close();
   }catch(...){}
   return -2;
  }

  //Now Truncate Status
  TruncateResPacket resPacket;
  try {
    socket.receiveBytes((void*)&resPacket,sizeof(TruncateResPacket));
  }catch(Exception &e){
    LOG(ERROR)<<"Error in receiving Truncate status packet"<<remoteFile<<": "<<e.message();
    try{
      socket.close();
    }catch(...){}
    return -2;
  }
  try {
    socket.close();
  } catch(...){}

  resPacket.statusCode = be64toh(resPacket.statusCode);
  return resPacket.statusCode;
}

int64_t BFSTcpServer::createRemoteFile(const std::string& remoteFile,
    const std::string& _ip, uint _port) {
  if(remoteFile.length() > 1024){
      LOG(ERROR)<<"Filename too long!";
    return false;
  }

  StreamSocket socket;
  try {
   SocketAddress addres(_ip,_port);
   socket.connect(addres);
   socket.setKeepAlive(false);
  }catch(Exception &e){
   LOG(ERROR)<<"Error in creating socket to:"<<_ip<<":"<<_port;
   return -2;
  }

  //Create a request
  CreateReqPacket createReqPacket;
  strncpy(createReqPacket.fileName,remoteFile.c_str(),remoteFile.length());
  createReqPacket.fileName[remoteFile.length()]= '\0';
  createReqPacket.opCode = htonl((uint32_t)BFS_REMOTE_OPERATION::CREATE);

  //Send Packet
  try {
   socket.sendBytes((void*)&createReqPacket,sizeof(CreateReqPacket));
  }catch(Exception &e){
   LOG(ERROR)<<"Error in sending Create request"<<remoteFile<<": "<<e.message();
   try {
     socket.close();
   }catch(...){}
   return -2;
  }

  //Now Create Status
  CreateResPacket resPacket;
  try {
    socket.receiveBytes((void*)&resPacket,sizeof(CreateResPacket));
  }catch(Exception &e){
    LOG(ERROR)<<"Error in receiving Create status packet"<<remoteFile<<": "<<e.message();
    try{
      socket.close();
    }catch(...){}
    return -2;
  }

  try {
    socket.close();
  } catch(...){}

  resPacket.statusCode = be64toh(resPacket.statusCode);
  return resPacket.statusCode;
}

int64_t BFSTcpServer::moveFileToRemoteNode(const std::string& file,
    const std::string& _ip, uint _port) {
  if(file.length() > 1024){
      LOG(ERROR)<<"Filename too long!";
    return false;
  }

  StreamSocket socket;
  try {
   SocketAddress addres(_ip,_port);
   socket.connect(addres);
   socket.setKeepAlive(false);
  }catch(Exception &e){
   LOG(ERROR)<<"Error in creating socket to:"<<_ip<<":"<<_port;
   return -2;
  }

  //Create a request
  MoveReqPacket moveReqPacket;
  strncpy(moveReqPacket.fileName,file.c_str(),file.length());
  moveReqPacket.fileName[file.length()]= '\0';
  moveReqPacket.opCode = htonl((uint32_t)BFS_REMOTE_OPERATION::MOVE);

  //Send Packet
  try {
   socket.sendBytes((void*)&moveReqPacket,sizeof(MoveReqPacket));
  }catch(Exception &e){
   LOG(ERROR)<<"Error in sending Move request"<<file<<": "<<e.message();
   try {
     socket.close();
   }catch(...){}
   return -2;
  }

  //Now Move Status
  MoveResPacket resPacket;
  try {
    socket.receiveBytes((void*)&resPacket,sizeof(MoveResPacket));
  }catch(Exception &e){
    LOG(ERROR)<<"Error in receiving Move status packet"<<file<<": "<<e.message();
    try{
      socket.close();
    }catch(...){}
    return -2;
  }
  resPacket.statusCode = be64toh(resPacket.statusCode);
  if(resPacket.statusCode == 200) {//Success
    LOG(INFO)<<"Move to node:"<<_ip<<" successful: "<<file<< " Updating global view";
    BFS::ZooHandler::getInstance().requestUpdateGlobalView();
  }
  else
    LOG(ERROR)<<"Move to remote node failed: "<<file<<" Returned Code:"<<resPacket.statusCode;

  try {
    socket.close();
  } catch(...){}

  return resPacket.statusCode;
}

std::string BFSTcpServer::getIP() {
  if(ip=="")
    initialize();
  return ip;
}
void BFSTcpServer::findIP(){
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct ifreq ifr;
  ifr.ifr_addr.sa_family = AF_INET;
  strncpy(ifr.ifr_name , iface.c_str() , IFNAMSIZ-1);
  ioctl(fd, SIOCGIFADDR, &ifr);
  close(fd);
  ip = string(inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
  printf("IP: %s\n",ip.c_str());
}

std::uint32_t BFSTcpServer::getPort() {
  if(port == 0)
    initialize();
  return port;
}

bool BFSTcpServer::initialize() {
  string devName = BFS::SettingManager::get(BFS::CONFIG_KEY_ZERO_NETWORK_DEV);
  if(devName.length() > 0){
    iface = devName;
    findIP();
    string portStr = BFS::SettingManager::get(BFS::CONFIG_KEY_TCP_PORT);
    if(!portStr.length()){
      LOG(DEBUG) <<"No tcp_port in the config file! \n"
          "Initializing BFSTCPServer failed.\n";
      return false;
    }
    port = stoul(portStr);
  } else{
    LOG(DEBUG) <<"No device specified in the config file! \n"
        "Initializing BFSTCPServer failed.\n";
    return false;
  }

  return true;
}



void BFSTcpServer::transferLoop() {
  while(isRunning) {
    TransferTask* front = transferQueue.front();

    if(unlikely(!isRunning))
      break;

    processTransfer(front);

    delete front;
    front = nullptr;
    transferQueue.pop();
  }
  LOG(ERROR)<<"TCP Transfer LOOP DEAD!";
}

void BFSTcpServer::processTransfer(TransferTask* _task) {
  string fileName = string(_task->writeReq.fileName);
  //Find file and lock it!
  BFS::FileNode* fNode = BFS::FileSystem::getInstance().findAndOpenNode(fileName);
  if(fNode == nullptr) {
    LOG(ERROR)<<"File Not found:"<<fileName;
    return;
  }
  uint64_t inodeNum = BFS::FileSystem::getInstance().assignINodeNum((intptr_t)fNode);
  //Write data to file
  BFS::FileNode* afterMove = nullptr;//This will happen
  long result = fNode->writeHandler((char*)_task->data,_task->writeReq.offset,_task->writeReq.size,afterMove,true);

  //Write happened LOCALLY! SOME free space come from somewhere :D
  if(result == (int64_t)_task->writeReq.size && !afterMove){
    //Set istransfering to false and close it
    LOG(INFO)<<"No Transfer is necessary for:"<<fileName<<" write happened locally!";
    fNode->setTransfering(false);
    fNode->close(inodeNum);
  }else if (result == (int64_t)_task->writeReq.size && afterMove) {//Success
    BFS::ZooHandler::getInstance().publishListOfFilesSYNC();
    BFS::ZooHandler::getInstance().requestUpdateGlobalView();
    LOG(INFO)<<"Transfer Successful:"<<fileName<<" from:"<<BFS::ZooHandler::getInstance().getHostName()<<" to:"<<afterMove->getRemoteHostIP();
    //We don't need to close file here because it's a newly created node;
    //however, move will make it open! so WE NEED TO CLOSE IT
    afterMove->close(0);
  } else {
    string afterMoveVal = "nullptr";
    string fNodeVal = "nullptr";
    if(afterMove)
      afterMoveVal = to_string((intptr_t)afterMove);
    if(fNode)
      fNodeVal = to_string((intptr_t)fNode);
    LOG(ERROR)<<"Unsuccessful transfer:"<<fileName<<" MEMUTILIZATION:"<<
        BFS::MemoryContorller::getInstance().getMemoryUtilization()<<
        " size:"<<_task->writeReq.size<<" written:"<<result<<
        " afterMoveNode:"<<afterMoveVal<<" fNode:"<<fNodeVal;
    fNode->setTransfering(false);
    fNode->close(inodeNum);//close the file
  }
}

void BFSTcpServer::addTransferTask(TransferTask* _task) {
  transferQueue.push(_task);
}
