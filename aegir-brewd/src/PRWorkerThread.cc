
#include "PRWorkerThread.hh"

#include <stdio.h>
#include <time.h>

#include <algorithm>
#include <vector>
#include <set>
#include <string>
#include <limits>

#include "JSONMessage.hh"
#include "ProcessState.hh"
#include "ElapsedTime.hh"

namespace aegir {

  static std::string jsvt2str(Json::ValueType _jsvt) {
    std::string typestr;
    switch(_jsvt) {
    case Json::ValueType::nullValue:
      typestr = "null";
      break;
    case Json::ValueType::intValue:
      typestr = "int";
      break;
    case Json::ValueType::uintValue:
      typestr = "uint";
      break;
    case Json::ValueType::realValue:
      typestr = "real";
      break;
    case Json::ValueType::stringValue:
      typestr = "string";
      break;
    case Json::ValueType::booleanValue:
      typestr = "boolean";
      break;
    case Json::ValueType::arrayValue:
      typestr = "array";
      break;
    case Json::ValueType::objectValue:
      typestr = "object";
      break;
    }
    return typestr;
  }

  static std::string epoch2iso(uint32_t _time) {
    std::string ret(24, 0);

    time_t clk = _time;
    struct tm tm;
    gmtime_r(&clk, &tm);
    size_t x = strftime((char*)ret.data(), ret.size()-1, "%G-%m-%dT%TZ", &tm);
    ret.resize(x);

    return ret;
  }

  PRWorkerThread::PRWorkerThread(std::string _name): c_name(_name),
						     c_mq_prw(ZMQ::SocketType::REP),
						     c_mq_iocmd(ZMQ::SocketType::PUB) {
    // Load the JSON Message handlers
    c_handlers["loadProgram"] = std::bind(&PRWorkerThread::handleLoadProgram, this, std::placeholders::_1);
    c_handlers["getProgram"] = std::bind(&PRWorkerThread::handleGetLoadedProgram, this, std::placeholders::_1);
    c_handlers["getState"] = std::bind(&PRWorkerThread::handleGetState, this, std::placeholders::_1);
    c_handlers["buzzer"] = std::bind(&PRWorkerThread::handleBuzzer, this, std::placeholders::_1);
    c_handlers["hasMalt"] = std::bind(&PRWorkerThread::handleHasMalt, this, std::placeholders::_1);
    c_handlers["resetProcess"] = std::bind(&PRWorkerThread::handleResetProcess, this, std::placeholders::_1);
    c_handlers["getVolume"] = std::bind(&PRWorkerThread::handleGetVolume, this, std::placeholders::_1);
    c_handlers["setVolume"] = std::bind(&PRWorkerThread::handleSetVolume, this, std::placeholders::_1);
    c_handlers["getTempHistory"] = std::bind(&PRWorkerThread::handleGetTempHistory, this, std::placeholders::_1);

    // connect the IO socket
    c_mq_iocmd.connect("inproc://iocmd");

    // connect the worker socket
    c_mq_prw.connect("inproc://prworkers");

    auto thrmgr = ThreadManager::getInstance();
    thrmgr->addThread(c_name, *this);
  }

  PRWorkerThread::~PRWorkerThread() {
  }

  void PRWorkerThread::run() {
    printf("PRWorkerThread %s started\n", c_name.c_str());

    std::chrono::microseconds ival(20000);
    std::shared_ptr<Message> msg;
    while (c_run) {
      while ( (msg = c_mq_prw.recv(ZMQ::MessageFormat::JSON)) ) {
	// if it's not a JSON type, then send an error response
	try {
	  if ( msg->type() != MessageType::JSON ) {
	    printf("PRWorkerThread: Got a non-JSON message\n");
	    Json::Value root;
	    root["status"] = "error";
	    root["message"] = "Not a JSON Message";
	    c_mq_prw.send(JSONMessage(root));
	    continue;
	  }
	  // Handling the JSON message
	  auto jsonmsg = std::static_pointer_cast<JSONMessage>(msg);
	  auto reply = handleJSONMessage(jsonmsg->getJSON());
	  c_mq_prw.send(JSONMessage(*reply));
	}
	catch (Exception &e) {
	  printf("PRWorkerThread: Exception while handling zmq message: %s\n", e.what());
	  Json::Value root;
	  root["status"] = "error";
	  root["message"] = e.what();
	  c_mq_prw.send(JSONMessage(root));
	}
	catch (std::exception &e) {
	  printf("PRWorkerThread: Exception while handling zmq message: %s\n", e.what());
	  Json::Value root;
	  root["status"] = "error";
	  root["message"] = e.what();
	  c_mq_prw.send(JSONMessage(root));
	}
	catch (...) {
	  printf("PRWorkerThread: unknown exception while recv zmq message\n");
	  Json::Value root;
	  root["status"] = "error";
	  root["message"] = "Unknown exception";
	  c_mq_prw.send(JSONMessage(root));
	}
      } // c_mq_recv
      std::this_thread::sleep_for(ival);
    }
    printf("PRWorkerThread %s stopped\n", c_name.c_str());
  }

  std::shared_ptr<Json::Value> PRWorkerThread::handleJSONMessage(const Json::Value &_msg) {

    if ( _msg.type() != Json::ValueType::objectValue ) {
      std::string typestr("unknown");
      switch(_msg.type()) {
      case Json::ValueType::nullValue:
	typestr = "null";
	break;
      case Json::ValueType::intValue:
	typestr = "int";
	break;
      case Json::ValueType::uintValue:
	typestr = "uint";
	break;
      case Json::ValueType::realValue:
	typestr = "real";
	break;
      case Json::ValueType::stringValue:
	typestr = "string";
	break;
      case Json::ValueType::booleanValue:
	typestr = "boolean";
	break;
      case Json::ValueType::arrayValue:
	typestr = "array";
	break;
      case Json::ValueType::objectValue:
	typestr = "object";
	break;
      }
      Json::Value root;
      root["status"] = "error";
      root["message"] = std::string("Input's type must be an object, and not a ") + typestr;
      return std::make_shared<Json::Value>(root);
    }

    // check whether we have the "command" defined
    if ( !_msg.isMember("command") ) {
      printf("PRWorkerThread::handleJSONMessage(): command is not defined\n");
      Json::Value root;
      root["status"] = "error";
      root["message"] = "command not supplied";
      return std::make_shared<Json::Value>(root);
    }

    // check whether command is a string
    auto cmd = _msg["command"];
    if ( !cmd.isString() ) {
      Json::Value root;
      root["status"] = "error";
      root["message"] = "command not a string";
      return std::make_shared<Json::Value>(root);
    }
    // fetch the command
    std::string cmdstr = cmd.asString();

    // now check whether we have data
    if ( !_msg.isMember("data") ) {
      Json::Value root;
      root["status"] = "error";
      root["message"] = "data is not supplied";
      return std::make_shared<Json::Value>(root);
    }

    // Check whether the handler for the command is defined
    auto cmdit = c_handlers.find(cmdstr);
    if ( cmdit == c_handlers.end() ) {
      Json::Value root;
      root["status"] = "error";
      root["message"] = "Unknown command";
      return std::make_shared<Json::Value>(root);
    }

    try {
      ElapsedTime t("PRWorkerThread handle "+cmdstr);
      return cmdit->second(_msg["data"]);
    }
    catch (Exception &e) {
      Json::Value root;
      root["status"] = "error";
      root["message"] = e.what();
      return std::make_shared<Json::Value>(root);
    }
    catch (std::exception &e) {
      Json::Value root;
      root["status"] = "error";
      root["message"] = e.what();
      return std::make_shared<Json::Value>(root);
    }
    catch (...) {
      Json::Value root;
      root["status"] = "error";
      root["message"] = "Unknown error";
      return std::make_shared<Json::Value>(root);
    }
    // clang nicely realizes that we won't reach this point, ever
  }

  /*
  'startat': 1492616760.0,
  'volume': 35}
   */
  std::shared_ptr<Json::Value> PRWorkerThread::handleLoadProgram(const Json::Value &_data) {

    if ( _data.type() != Json::ValueType::objectValue )
      throw Exception("Data type should be an objectvalue");
    // first get the start time and the volume
    uint32_t startat;
    uint32_t volume;

    // check for required members in the data struct
    for ( auto &it: std::vector<std::string>({"program", "startat", "volume"}) ) {
      if ( !_data.isMember(it) )
	throw Exception("Missing member in data: %s", it.c_str());
    }

    Json::Value jsonvalue = _data["startat"];
    if ( jsonvalue.type() != Json::ValueType::intValue &&
	 jsonvalue.type() != Json::ValueType::uintValue )
      throw Exception("data.startat must be an integer type and not %s", jsvt2str(jsonvalue.type()).c_str());
    startat = jsonvalue.asUInt();

    jsonvalue = _data["volume"];
    if ( jsonvalue.type() != Json::ValueType::intValue &&
	 jsonvalue.type() != Json::ValueType::uintValue )
      throw Exception("data.startat must be an integer type and not %s", jsvt2str(jsonvalue.type()).c_str());
    volume = jsonvalue.asUInt();

    //printf("Startat:%u Volume:%u\n", startat, volume);

    //verify the time
    time_t now = time(0);
    time_t minbefore = now - 600;
    time_t maxahead = now + 3600*168;
    if ( startat && startat < minbefore )
      throw Exception("Startat is too much in the past");

    if ( startat > maxahead )
      throw Exception("Startat is too much in the future");

    if ( startat < now ) startat = 0;
    // verifying startat finished

    // verify the volume
    if ( volume > 70 )
      throw Exception("volume is too large");

    // now check the program
    /*
     {'program': {'boiltime': 60,
              'endtemp': 80,
              'hops': [{'attime': 60, 'hopname': 'Hop 2', 'hopqty': 2},
                       {'attime': 1, 'hopname': 'Hop 1', 'hopqty': 1},
                       {'attime': 0, 'hopname': 'Hop 3 ', 'hopqty': 3}],
              'id': 4,
              'mashsteps': [{'holdtime': 15, 'orderno': 0, 'temperature': 42},
                            {'holdtime': 15, 'orderno': 1, 'temperature': 63},
                            {'holdtime': 15, 'orderno': 2, 'temperature': 69}],
              'name': 'Test 1',
              'starttemp': 37},
     */
    float prog_starttemp, prog_endtemp;
    uint16_t prog_boiltime;
    uint32_t prog_id;
    Program::Hops prog_hops;
    Program::MashSteps prog_mashsteps;
    Json::Value jprog = _data["program"];
    {
      for (auto &it: std::set<std::string>({"boiltime", "endtemp", "starttemp", "id", "hops", "mashsteps"}) ) {
	if ( !jprog.isMember(it) )
	  throw Exception("Program missing member: %s", it.c_str());
      }
      // get the id
      jsonvalue = jprog["id"];
      prog_id = jsonvalue.asUInt();

      // get the boiltime
      jsonvalue = jprog["boiltime"];
      prog_boiltime = jsonvalue.asUInt();

      // start and end temp
      jsonvalue = jprog["starttemp"];
      prog_starttemp = jsonvalue.asFloat();
      jsonvalue = jprog["endtemp"];
      prog_endtemp = jsonvalue.asFloat();

      // now get the hoppings
      Json::Value jarray = jprog["hops"];
      if ( jarray.type() != Json::ValueType::arrayValue )
	throw Exception("data.program.hops must be an array");

      for (auto &it: jarray ) {
	if ( it.type() != Json::ValueType::objectValue )
	  throw Exception("data.program.hops members must be of an object type");
	Program::Hop hop;
	// attime
	hop.attime = it["attime"].asUInt();
	// hop's id
	hop.id = it["id"].asUInt();

	if ( hop.attime > prog_boiltime )
	  throw Exception("Hop's time cannot be larger than the boiltime");

	// and finally add it
	prog_hops.push_back(hop);
      } // it: jarray

      // and the mash steps
      jarray = jprog["mashsteps"];
      if ( jarray.type() != Json::ValueType::arrayValue )
	throw Exception("data.program.mashsteps must be an array");

      prog_mashsteps.resize(jarray.size());
      for ( auto &it: jarray ) {
	if ( it.type() != Json::ValueType::objectValue )
	  throw Exception("data.program.hops members must be of an object type");

	uint32_t orderno = it["orderno"].asUInt();
	if ( orderno >= jarray.size() )
	  throw Exception("data.program.mashsteps orderno out of bounds");

	float temp = it["temperature"].asFloat();
	prog_mashsteps[orderno].orderno  = orderno;
	prog_mashsteps[orderno].temp     = temp;
	prog_mashsteps[orderno].holdtime = it["holdtime"].asUInt();
	if ( temp < prog_starttemp || temp > prog_endtemp )
	  throw Exception("Mashstep temperature out of bounds");
      } // it: jarray
    }
    //printf("id:%u boiltime:%u ST:%f ET:%f\n", prog_id, prog_boiltime, prog_starttemp, prog_endtemp);

    Program prog(prog_id, prog_starttemp, prog_endtemp, prog_boiltime, prog_mashsteps, prog_hops);
    ProcessState::getInstance().loadProgram(prog, startat, volume);

    // the success reply
    Json::Value resp;
    resp["status"] = "success";
    resp["message"] = "Command Loaded";
    return std::make_shared<Json::Value>(resp);
  }

  std::shared_ptr<Json::Value> PRWorkerThread::handleGetLoadedProgram(const Json::Value &_data) {
    try {
      std::shared_ptr<Program> prog = ProcessState::getInstance().getProgram();
      if ( prog == nullptr ) {
	throw Exception("No program loaded");
      }
      Json::Value resp;
      resp["status"] = "success";
      resp["data"] = Json::Value();
      resp["data"]["progid"] = prog->getId();
      return std::make_shared<Json::Value>(resp);
    }
    catch (Exception &e) {
      Json::Value resp;
      resp["status"] = "error";
      resp["message"] = e.what();
      return std::make_shared<Json::Value>(resp);
    }
    catch (std::exception &e) {
      Json::Value resp;
      resp["status"] = "error";
      resp["message"] = e.what();
      return std::make_shared<Json::Value>(resp);
    }
    catch (...) {
      Json::Value resp;
      resp["status"] = "error";
      resp["message"] = "Unknown error";
      return std::make_shared<Json::Value>(resp);
    }
    // shouldn't be reached
  }

  std::shared_ptr<Json::Value> PRWorkerThread::handleGetState(const Json::Value &_data) {
    // do these initializations ahead of the mutex lock to avoid wasting mutex time
    Json::Value retval;
    retval["status"] = "success";
    retval["data"] = Json::Value();

    Json::Value data, jsval;
    std::set<std::string> tcs;
    ProcessState::ThermoDataPoints tcvals;

    ProcessState &ps(ProcessState::getInstance());

    ProcessState::Guard guard_ps(ps);

    try {
      // first get the current state
      data["state"] = ps.getStringState();

      // Thermo readings
      ps.getThermoCouples(tcs);
      for ( auto &it: tcs ) {
	// Add the current sensor temps
	data["currtemp"][it] = ps.getSensorTemp(it);
      }

      // Add the current target temperature
      data["targettemp"] = ps.getTargetTemp();

      // If we have a loaded program, return its id
      if ( ps.getState() >= ProcessState::States::Loaded ) {
	data["programid"] = ps.getProgram()->getId();
      }

      // If we're mashing, then display the current step
      if ( ps.getState() == ProcessState::States::Mashing ) {
	Json::Value jms;
	time_t now = time(0);
	int8_t step = ps.getMashStep();
	time_t start = ps.getMashStepStart();
	time_t diff = now - start;

	jms["orderno"] = step;
	jms["time"] = diff<0 ? 0 : diff;
	data["mashstep"] = jms;
      }
    }
    catch (Exception &e) {
      Json::Value resp;
      resp["status"] = "error";
      resp["message"] = e.what();
      return std::make_shared<Json::Value>(resp);
    }
    catch (std::exception &e) {
      Json::Value resp;
      resp["status"] = "error";
      resp["message"] = e.what();
      return std::make_shared<Json::Value>(resp);
    }
    catch (...) {
      Json::Value resp;
      resp["status"] = "error";
      resp["message"] = "Unknown error";
      return std::make_shared<Json::Value>(resp);
    }
    retval["data"] = data;

    return std::make_shared<Json::Value>(retval);
  }

  /*
    required: state
    if state==pulsate: cycletime, onratio
   */
  std::shared_ptr<Json::Value> PRWorkerThread::handleBuzzer(const Json::Value &_data) {
    Json::Value retval;
    retval["status"] = "success";
    retval["data"] = Json::Value();

    if ( _data.type() != Json::ValueType::objectValue )
      throw Exception("Data type should be an objectvalue");
    std::string strstate;
    PINState state(PINState::Unknown);
    float cycletime(0.2), onratio(0.2);

    // check for required members in the data struct
    for ( auto &it: std::vector<std::string>({"state"}) ) {
      if ( !_data.isMember(it) )
	throw Exception("Missing member in data: %s", it.c_str());
    }

    Json::Value jsonvalue = _data["state"];
    if ( jsonvalue.type() != Json::ValueType::stringValue )
      throw Exception("data.state must be a string type, and not %s", jsvt2str(jsonvalue.type()).c_str());
    strstate = jsonvalue.asString();
    std::transform(strstate.begin(), strstate.end(), strstate.begin(),
		   [](char c) {return std::tolower(c);});

    if ( strstate == "on" ) {
      state = PINState::On;
    } else if ( strstate == "off" ) {
      state = PINState::Off;
    } else if ( strstate == "pulsate" ) {
      state = PINState::Pulsate;
    } else {
      throw Exception("Unknown state: %s", strstate.c_str());
    }

    // get the additional parameters for pulsate
    if ( state == PINState::Pulsate ) {
      for ( auto &it: std::vector<std::string>({"cycletime", "onratio"}) ) {
	if ( !_data.isMember(it) )
	  throw Exception("Missing member in data: %s", it.c_str());
      }

      jsonvalue = _data["cycletime"];
      if ( !jsonvalue.isNumeric() )
	throw Exception("data.cycletime must be a numeric type, and not %s", jsvt2str(jsonvalue.type()).c_str());

      cycletime = jsonvalue.asFloat();

      jsonvalue = _data["onratio"];
      if ( !jsonvalue.isNumeric() )
	throw Exception("data.onratio must be a numeric type, and not %s", jsvt2str(jsonvalue.type()).c_str());

      onratio = jsonvalue.asFloat();
    }

    c_mq_iocmd.send(PinStateMessage("buzzer", state, cycletime, onratio));

    return std::make_shared<Json::Value>(retval);
  }

  std::shared_ptr<Json::Value> PRWorkerThread::handleHasMalt(const Json::Value &_data) {
    Json::Value retval;
    retval["status"] = "success";
    retval["data"] = Json::Value();

    ProcessState &ps(ProcessState::getInstance());
    ProcessState::Guard guard_ps(ps);
    if ( ps.getState() != ProcessState::States::NeedMalt ) {
      throw Exception("Only valid at state NeedMalt");
    }

    ps.setState(ProcessState::States::Mashing);

    return std::make_shared<Json::Value>(retval);
  }

  std::shared_ptr<Json::Value> PRWorkerThread::handleResetProcess(const Json::Value &_data) {
    Json::Value retval;
    retval["status"] = "success";
    retval["data"] = Json::Value();

    ProcessState &ps(ProcessState::getInstance());
    ProcessState::Guard guard_ps(ps);
    if ( ps.getState() == ProcessState::States::Empty) {
      throw Exception("Not valid when Empty");
    }

    ps.reset();

    return std::make_shared<Json::Value>(retval);
  }

  std::shared_ptr<Json::Value> PRWorkerThread::handleGetVolume(const Json::Value &_data) {
    ProcessState &ps(ProcessState::getInstance());

    // we cannot get the volume while there's no program loaded
    if ( ps.getState() == ProcessState::States::Empty ) {
      throw Exception("Cannot get volume while Empty");
    }
    Json::Value retval;
    retval["status"] = "success";
    retval["data"] = Json::Value(Json::ValueType::objectValue);
    retval["data"]["volume"] = ps.getVolume();

    return std::make_shared<Json::Value>(retval);
  }

  std::shared_ptr<Json::Value> PRWorkerThread::handleSetVolume(const Json::Value &_data) {
    ProcessState &ps(ProcessState::getInstance());

    // cannot set the volume when there's no program loaded
    if ( ps.getState() == ProcessState::States::Empty ) {
      throw Exception("Cannot set volume while Empty");
    }

    // verify the input
    if ( !_data.isMember("volume") ) {
      throw Exception("Need the volume in data");
    }

    Json::Value jsonvalue = _data["volume"];
    if ( !jsonvalue.isNumeric() ) {
      throw Exception("volume must be numeric");
    }

    uint32_t volume = jsonvalue.asUInt();

    if ( volume < 5 || volume > 80 ) {
      throw Exception("Volume is out of bounds");
    }

    ps.setVolume(volume);

    // return success
    Json::Value retval;
    retval["status"] = "success";
    retval["data"] = Json::Value(Json::ValueType::nullValue);

    return std::make_shared<Json::Value>(retval);
  }

  std::shared_ptr<Json::Value> PRWorkerThread::handleGetTempHistory(const Json::Value &_data) {
    ProcessState &ps(ProcessState::getInstance());

    // only valid from mashing
    if ( ps.getState() < ProcessState::States::Mashing ) {
      throw Exception("Cannot be used before Mashing");
    }

    // verify the input
    uint32_t from = 0;
    if ( _data.isMember("from") ) {
      Json::Value jsonvalue = _data["from"];
      if ( !jsonvalue.isNumeric() ) {
	throw Exception("field 'from' must be numeric");
      }

      from = jsonvalue.asUInt();
    }

    Json::Value retval;

    std::set<std::string> historytcs{"MashTun", "RIMS"};

    uint32_t maxtime = std::numeric_limits<uint32_t>::max();
    // first, get all our TC readings, and calculate the max time we have currently
    std::map<std::string, ProcessState::ThermoDataPoints> tcvals;

    // fetch the values
    // ProcesState is only locked during this
    {
      std::set<std::string> tcs;

      ProcessState::Guard guard_ps(ps);
      ps.getThermoCouples(tcs);
      for ( auto &it: tcs ) {
	// we only return the graphed TCs here
	if ( historytcs.find(it) == historytcs.end() ) continue;
	tcvals[it] = ProcessState::ThermoDataPoints();
	ps.getTCReadings(it, tcvals[it]);
      }
    }

    // now build up the indexes
    std::set<uint32_t> indexes;
    for (auto &tcit: tcvals) {
      for (auto &valit: tcit.second) {
	indexes.insert(valit.first);
      }
    }

    // create the structure
    // one for the timestamps, and under .readings one for each TC
    Json::Value th;
    th["timestamps"] = Json::Value(Json::ValueType::arrayValue);
    th["readings"] = Json::Value(Json::ValueType::objectValue);

    // create the array JSON object type for each returnd TC
    for ( auto &it: tcvals )
      th["readings"][it.first] = Json::Value(Json::ValueType::arrayValue);


    int maxcnt(512); // max ammount we return in a single call
    auto it = indexes.begin();
    if ( from > 0 ) {
      // start at the point
      it = indexes.find(from);
      // if not found, then start at the next available one
      if ( it == indexes.end() ) {
	for (it = indexes.begin(); *it <= from; ++it);
      }
    }
    // the index iterator is positioned for the start of the batch
    ProcessState::ThermoDataPoints::iterator tdit;
    for (int i=0; it != indexes.end() && i < maxcnt; ++it, ++i) {
      // add the timestamp to the vector
      th["timestamps"].append(*it);

      // look up each TC's data
      for (auto &tcit: tcvals) {
	if ( (tdit = tcit.second.find(*it)) != tcit.second.end() ) {
	  th["readings"][tcit.first].append(tdit->second);
	} else {
	  th["readings"][tcit.first].append(Json::Value(Json::ValueType::nullValue));
	}
      }
    }

    // return success
    retval["status"] = "success";
    retval["data"] = th;

    return std::make_shared<Json::Value>(retval);
  }
}