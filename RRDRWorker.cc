#include "RRDRWorker.hh"

void RRDRWorker::doProcess() {
  string lostBlkName, localBlkName;
  unsigned int nextIP, prevIP, requestorIP;
  const char* cmd;
  unsigned int lostBlkNameLen, localBlkNameLen;
  struct timeval tv1, tv2;
  _ecK = 3;
  while (true) {
    // loop FOREVER
    cout << "waiting for cmds" << endl;
    redisReply* rReply = (redisReply*)redisCommand(_selfCtx, "blpop dr_cmds 0");
    if (rReply -> type == REDIS_REPLY_NIL) {
      if (DR_WORKER_DEBUG) cout << "RRDRWorker::doProcess(): empty list" << endl;
      freeReplyObject(rReply);
    } else if (rReply -> type == REDIS_REPLY_ERROR) {
      if (DR_WORKER_DEBUG) cout << "RRDRWorker::doProcess(): error happens" << endl;
      freeReplyObject(rReply);
    } else {
      gettimeofday(&tv1, NULL);
      cout << "recv'd cmd" << tv1.tv_sec << "s" << tv1.tv_usec << "us" << endl;
      /** 
       * Parsing Cmd
       *
       * Cmd format: 
       * [a(4Byte)][b(4Byte)][c(4Byte)][d(4Byte)][e(4Byte)][f(?Byte)][g(?Byte)]
       * a: ecK pos: 0 // if ((ecK & 0xff00) >> 1) == 1), requestor is a holder
       * b: requestor ip start pos: 4
       * c: prev ip start pos 8 // not used
       * d: next ip start pos 12
       * e: id pos 16
       * f: lost file name (4Byte lenght + length) start pos 20, 24
       * g: corresponding filename in local start pos ?, ? + 4
       */
      cmd = rReply -> element[1] -> str;
      memcpy((char*)&_ecN, cmd, 4);
      memcpy((char*)&requestorIP, cmd + 4, 4);
      memcpy((char*)&prevIP, cmd + 8, 4);
      memcpy((char*)&nextIP, cmd + 12, 4);
      memcpy((char*)&_id, cmd + 16, 4);
      _ecN+=1;
      // get file names
      memcpy((char*)&lostBlkNameLen, cmd + 20, 4);

      _coefficient = (lostBlkNameLen >> 16);
      lostBlkNameLen = (lostBlkNameLen & 0xffff);

      lostBlkName = string(cmd + 24, lostBlkNameLen);
      memcpy((char*)&localBlkNameLen, cmd + 24 + lostBlkNameLen, 4);
      localBlkName = string(cmd + 28 + lostBlkNameLen, localBlkNameLen);

      //if (DR_WORKER_DEBUG) {
        cout << "lostBlkName: " << lostBlkName << endl
          << " localBlkName: " << localBlkName << endl
          << " id: " << _id  << endl
          << " ecN: " << _ecN << endl
          << " requestorIP: " << ip2Str(requestorIP) << endl
          << " prevIP: " << ip2Str(prevIP) << endl
          << " nextIP: " << ip2Str(nextIP) << endl;
      //}

      _toSendCnt = 0;
      _readCnt = 0;

      thread diskThread([=]{reader(localBlkName, _selfCtx);});
      freeReplyObject(rReply);


      ///******************* 
      // * Let's ROOOOCK!!!
      // *******************/

      thread sendThread([=]{sender(lostBlkName, _selfCtx);});

      puller(lostBlkName, findCtx(prevIP));

      diskThread.join();
      sendThread.join();
      gettimeofday(&tv2, NULL);
      //if (DR_WORKER_DEBUG) 
        cout << "RRDRWorker::doProcess() start at " << tv1.tv_sec << "." << tv1.tv_usec
          << " end at " << tv2.tv_sec << "." << tv2.tv_usec << endl;
      cleanup();
    }
  }
  ;
}

void RRDRWorker::sender(const string& filename, redisContext* rc) {
  redisReply* rReply;
  cout << "sender() start" << endl;
  struct timeval tv1, tv2;
  

  int lastGroupCnt = _packetCnt % (_ecN - 1), lastGroupBase;
  if (lastGroupCnt == 0) lastGroupCnt = _ecN - 1;
  lastGroupBase = _packetCnt - lastGroupCnt;

  for (int i = 0; i < _packetCnt; i ++) {
    while (i >= _toSendCnt) {
      cout << "sender(): i = " << i << ", _toSendCnt = " << _toSendCnt << endl;
      unique_lock<mutex> lck(_toSenderMtx);
      _toSenderCV.wait(lck);
    }
    cout << "sender(): after mutes" << endl;
    if (_id != _ecN - 1 && ((i) % (_ecN - 1))  == _id) {
      // to req
      rReply = (redisReply*)redisCommand(rc, 
          "RPUSH %s:%d %b", 
          filename.c_str(), i, _diskPkts[i], _packetSize);
      cout << "sender(): to req " << i << " target idx " << i  << endl;
      freeReplyObject(rReply);
    } else if(((i -_id + _ecN - 1) % (_ecN - 1)) < _ecK) {
      // to next 
      gettimeofday(&tv1, NULL);
      cout << tv1.tv_sec << "s" << tv1.tv_usec << "us, sender(): before sending to next " << i << endl;
      rReply = (redisReply*)redisCommand(rc, 
          "RPUSH tmp:%s:%d %b", 
          filename.c_str(), i,_diskPkts[i], _packetSize);
      freeReplyObject(rReply);
      gettimeofday(&tv2, NULL);
      cout << "send duration: " << ((tv2.tv_sec - tv1.tv_sec) * 1000000.0 + tv2.tv_usec - tv1.tv_usec) / 1000000 << endl;
    }
  }
/*
  cout << "sender(): to deal with last packet" << endl;
  while (_packetCnt - 1 >= _toSendCnt) {
    unique_lock<mutex> lck(_toSenderMtx);
    _toSenderCV.wait(lck);
  }

  cout << "sender(): dealing with last packet" << endl;

  if (_id < lastGroupCnt) {
    rReply = (redisReply*)redisCommand(rc, 
        "RPUSH %s:%d %b", 
        filename.c_str(), _id + lastGroupBase, 
        _diskPkts[_packetCnt - 1], _packetSize);
    cout << "sender(): to req " << _packetCnt - 1 << " target idx " << _id + _packetCnt / (_ecN - 1) * (_ecN - 1) << endl;
    freeReplyObject(rReply);
  } else {
    gettimeofday(&tv1, NULL);
    cout << tv1.tv_sec << "s" << tv1.tv_usec << "us, sender(): before sending to next " << endl;
    rReply = (redisReply*)redisCommand(rc, 
        "RPUSH tmp:%s:%d %b", 
        filename.c_str(), 
        _diskPkts[_packetCnt - 1], _packetCnt-1,_packetSize);
    freeReplyObject(rReply);
    gettimeofday(&tv2, NULL);
    cout << "send duration: " << ((tv2.tv_sec - tv1.tv_sec) * 1000000.0 + tv2.tv_usec - tv1.tv_usec) / 1000000 << endl;
  }
*/
}

void RRDRWorker::puller(const string& filename, redisContext* rc) {
  int i, groupSize = _ecN - 1;
  int retrieveCnt = _packetCnt;
  redisReply* rReply;
  struct timeval tv1;

  //if (_id != 0) {
  //  retrieveCnt -= _packetCnt / groupSize; 
  //  if (_id <= _packetCnt % groupSize) retrieveCnt --;
  //}

  cout << "puller() start " << endl;

  for (int i = 0; i < _packetCnt; i ++) {
    if (((i-_id + groupSize) % groupSize) < _ecK -1) {
    	redisAppendCommand(rc, "BLPOP tmp:%s:%d 0", filename.c_str(),i);
    }
  }
  cout << "puller() send command " << endl;

  for (i = 0; i < _packetCnt; i ++) {
    if (((i-_id + groupSize) % groupSize) < _ecK - 1 ) {
  	cout << "puller() wait previous results " <<i<<" and " <<_id<<" and " <<(i-_id)%groupSize<<endl;
      redisGetReply(rc, (void**)&rReply);
    }
    gettimeofday(&tv1, NULL);
    cout << tv1.tv_sec << "s" << tv1.tv_usec << "us puller(): recv'd packet " << i << endl;
    //cout << "puller(): i = " << i << ", _readCnt = " << _readCnt << endl;
    while (i >= _readCnt) {
      //cout << "puller(): i = " << i << ", _readCnt = " << _readCnt << endl;
      unique_lock<mutex> lck(_toPullerMtx);
      _toPullerCV.wait(lck);
    }
    //cout << "puller(): get read packet " << i << endl;
    //if (_id == 0 || (i % groupSize) != 0) {
      //cout << "puller(): before computing " << rReply -> elements << endl;
      //cout << "puller(): before computing " << rReply -> element[1] -> len << endl;
    if (((i-_id + groupSize) % groupSize) < _ecK - 1) {
      cout << "puller(): before computing " << rReply -> elements << endl;
      Computation::XORBuffers(_diskPkts[i], rReply -> element[1] -> str, _packetSize);
      //cout << "puller(): after computing " << endl;
    }


    _toSendCnt ++;
    unique_lock<mutex> lck(_toSenderMtx);
    _toSenderCV.notify_one();
    if (((i-_id + groupSize) % groupSize) < _ecK - 1) {
      freeReplyObject(rReply);
    }
  }
  cout << "puller finished" << endl;
}

void RRDRWorker::reader(const string& filename,  redisContext* rc) {
  redisReply* rReply;
  string fullName = _conf -> _blkDir + '/' + filename;
  int fd = open(fullName.c_str(), O_RDONLY);
  int groupSize = _ecN - 1, i, j, base = 0, readLen, readl;
  int round = _packetCnt % groupSize == 0 ? _packetCnt / groupSize : _packetCnt / groupSize + 1;

  vector<int> pktIdx;

  struct timeval tv1;
  gettimeofday(&tv1, NULL);
  cout << "reader() start " << tv1.tv_sec << "s" << tv1.tv_usec << "us" << endl;

  for (i = 0; i < round; i ++) {
    for (j = _id - 1; j >= 0; j --) if (base + j < _packetCnt) pktIdx.push_back(base + j);
    for (j = groupSize - 1; j >= 0; j --) if (base + j < _packetCnt) pktIdx.push_back(base + j); 
    base += groupSize;
  }

  for (i = 0; i < _packetCnt; i ++) {
    readLen = 0;
    while (readLen < _packetSize) {
      if ((readl = pread(fd, 
              _diskPkts[i] + readLen, 
              _packetSize - readLen, 
              pktIdx[i] * _packetSize)) < 0) {
        cerr << "ERROR During disk read" << endl;
      } else {
        readLen += readl;
      }
    }
    RSUtil::multiply(_diskPkts[i], _coefficient, _packetSize);
    /*if(((i -_id + _ecN - 1) % (_ecN - 1)) == _ecK-1) {
      // to next 
      rReply = (redisReply*)redisCommand(rc, 
          "RPUSH tmp:%s:%d %b", 
          filename.c_str(), i,_diskPkts[i], _packetSize);
      freeReplyObject(rReply);
    }
	*/
    _readCnt ++;
    gettimeofday(&tv1, NULL);
    cout << tv1.tv_sec << "s" << tv1.tv_usec << "us" << "reader() read packet " << i << ", readCnt " << _readCnt << endl;
    unique_lock<mutex> lck(_toPullerMtx);
    _toPullerCV.notify_one();
  }
  cout<<"reader over "<<endl;
}


