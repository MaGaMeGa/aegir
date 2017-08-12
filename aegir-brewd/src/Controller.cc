#include "Controller.hh"

#include <time.h>

#include <cmath>

#include <map>
#include <set>
#include <string>

#include "Exception.hh"

namespace aegir {
  /*
   * Controller
   */

  Controller *Controller::c_instance(0);

  Controller::Controller(): PINTracker(), c_mq_io(ZMQ::SocketType::SUB), c_ps(ProcessState::getInstance()),
			    c_mq_iocmd(ZMQ::SocketType::PUB), c_stoprecirc(false) {
    // subscribe to our publisher for IO events
    try {
      c_mq_io.connect("inproc://iopub").subscribe("");
      c_mq_iocmd.connect("inproc://iocmd");
    }
    catch (std::exception &e) {
      printf("Sub failed: %s\n", e.what());
    }
    catch (...) {
      printf("Sub failed: unknown exception\n");
    }

    c_cfg = Config::getInstance();

    // reconfigure state variables
    reconfigure();

    c_stagehandlers[ProcessState::States::Empty] = std::bind(&Controller::stageEmpty,
							     std::placeholders::_1, std::placeholders::_2);
    c_stagehandlers[ProcessState::States::Loaded] = std::bind(&Controller::stageLoaded
							      , std::placeholders::_1, std::placeholders::_2);
    c_stagehandlers[ProcessState::States::PreWait] = std::bind(&Controller::stagePreWait,
							       std::placeholders::_1, std::placeholders::_2);
    c_stagehandlers[ProcessState::States::PreHeat] = std::bind(&Controller::stagePreHeat,
							       std::placeholders::_1, std::placeholders::_2);
    c_stagehandlers[ProcessState::States::NeedMalt] = std::bind(&Controller::stageNeedMalt,
								std::placeholders::_1, std::placeholders::_2);
    c_stagehandlers[ProcessState::States::Mashing] = std::bind(&Controller::stageMashing,
							       std::placeholders::_1, std::placeholders::_2);
    c_stagehandlers[ProcessState::States::Sparging] = std::bind(&Controller::stageSparging,
								std::placeholders::_1, std::placeholders::_2);
    c_stagehandlers[ProcessState::States::PreBoil] = std::bind(&Controller::stagePreBoil,
							       std::placeholders::_1, std::placeholders::_2);
    c_stagehandlers[ProcessState::States::Hopping] = std::bind(&Controller::stageHopping,
							       std::placeholders::_1, std::placeholders::_2);
    c_stagehandlers[ProcessState::States::Cooling] = std::bind(&Controller::stageCooling,
							       std::placeholders::_1, std::placeholders::_2);
    c_stagehandlers[ProcessState::States::Finished] = std::bind(&Controller::stageFinished, std::placeholders::_1,
								std::placeholders::_2);

      // finally register to the threadmanager
    auto thrmgr = ThreadManager::getInstance();
    thrmgr->addThread("Controller", *this);
  }

  Controller::~Controller() {
  }

  Controller *Controller::getInstance() {
    if ( !c_instance) c_instance = new Controller();
    return c_instance;
  }

  void Controller::run() {
    printf("Controller started\n");

    // The main event loop
    std::shared_ptr<Message> msg;
    std::chrono::microseconds ival(20000);
    while ( c_run ) {

      // PINTracker's cycle
      startCycle();
      // read the GPIO pin channel first
      try {
	while ( (msg = c_mq_io.recv()) != nullptr ) {
	  if ( msg->type() == MessageType::PINSTATE ) {
	    auto psmsg = std::static_pointer_cast<PinStateMessage>(msg);
#ifdef AEGIR_DEBUG
	    printf("Controller: received %s:%hhu\n", psmsg->getName().c_str(), psmsg->getState());
#endif
	    try {
	      setPIN(psmsg->getName(), psmsg->getState());
	    }
	    catch (Exception &e) {
	      printf("No such pin: '%s' %lu\n", psmsg->getName().c_str(), psmsg->getName().length());
	      continue;
	    }
	  } else if ( msg->type() == MessageType::THERMOREADING ) {
	    auto trmsg = std::static_pointer_cast<ThermoReadingMessage>(msg);
#ifdef AEGIR_DEBUG
	    printf("Controller: got temp reading: %s/%f/%u\n",
		   trmsg->getName().c_str(),
		   trmsg->getTemp(),
		   trmsg->getTimestamp());
#endif
	    // add it to the process state
	    c_ps.addThermoReading(trmsg->getName(), trmsg->getTimestamp(), trmsg->getTemp());
	  } else {
	    printf("Got unhandled message type: %i\n", (int)msg->type());
	    continue;
	  }
	}
      } // End of pin and sensor readings
      catch (Exception &e) {
	printf("Exception: %s\n", e.what());
      }

      // handle the changes
      controlProcess(*this);

      // Send back the commands to IOHandler
      // PINTracker::endCycle() does this
      endCycle();

      // sleep a bit
      std::this_thread::sleep_for(ival);
    }

    printf("Controller stopped\n");
  }

  void Controller::reconfigure() {
    PINTracker::reconfigure();
    c_hecycletime = c_cfg->getHECycleTime();
  }

  void Controller::controlProcess(PINTracker &_pt) {
    ProcessState::Guard guard_ps(c_ps);
    ProcessState::States state = c_ps.getState();

    // The pump&heat control button
    if ( _pt.hasChanges() ) {
      std::shared_ptr<PINTracker::PIN> swon(_pt.getPIN("swon"));
      std::shared_ptr<PINTracker::PIN> swoff(_pt.getPIN("swoff"));
      // whether we have at least one of the controls
      if ( swon->isChanged() || swoff->isChanged() ) {
	if ( swon->getNewValue() != PINState::Off
	     && swoff->getNewValue() == PINState::Off ) {
	  setPIN("swled", PINState::On);
	  c_stoprecirc = true;
	} else if ( swon->getNewValue() == PINState::Off &&
		    swoff->getNewValue() != PINState::Off ) {
	  setPIN("swled", PINState::Off);
	  c_stoprecirc = false;
	}
      }
    } // pump&heat switch

    // state function
    auto it = c_stagehandlers.find(state);
    if ( it != c_stagehandlers.end() ) {
      it->second(this, _pt);
    } else {
      printf("No function found for state %hhu\n", state);
    }

    if ( c_stoprecirc ) {
      setPIN("rimspump", PINState::Off);
      setPIN("rimsheat", PINState::Off);
    } // stop recirc

  } // controlProcess

  void Controller::stageEmpty(PINTracker &_pt) {
    c_lastcontrol = 0;
  }

  void Controller::stageLoaded(PINTracker &_pt) {
    c_prog = c_ps.getProgram();
    // Loaded, so we should verify the timestamps
    uint32_t startat = c_ps.getStartat();
    c_preheat_phase = 0;
    c_hecycletime = c_cfg->getHECycleTime();
    // if we start immediately then jump to PreHeat
    if ( startat == 0 ) {
      c_ps.setState(ProcessState::States::PreHeat);
      c_lastcontrol = 0;
    } else {
      c_ps.setState(ProcessState::States::PreWait);
      c_lastcontrol = 0;
    }
    return;
  }

  void Controller::stagePreWait(PINTracker &_pt) {
    float mttemp = c_ps.getSensorTemp("MashTun");
    if ( mttemp == 0 ) return;
    // let's see how much time do we have till we have to start pre-heating
    uint32_t now = time(0);
    // calculate how much time
    float tempdiff = c_prog->getStartTemp() - mttemp;
    // Pre-Heat time
    // the time we have till pre-heating has to actually start
    uint32_t phtime = 0;

    if ( tempdiff > 0 )
      phtime = calcHeatTime(c_ps.getVolume(), tempdiff, 0.001*c_cfg->getHEPower());

    printf("Controller::controllProcess() td:%.2f PreHeatTime:%u\n", tempdiff, phtime);
    uint32_t startat = c_ps.getStartat();
    if ( (now + phtime*1.15) > startat ) {
      c_ps.setState(ProcessState::States::PreHeat);
      c_lastcontrol = 0;
    }
  }

  void Controller::stagePreHeat(PINTracker &_pt) {
    float mttemp = c_ps.getSensorTemp("MashTun");
    float rimstemp = c_ps.getSensorTemp("RIMS");
    float targettemp = c_prog->getStartTemp();
    float tempdiff = targettemp - mttemp;

#if 0
    int newphase = 0;

    if ( mttemp == 0 || rimstemp == 0 ) {
      newphase = 0;
    } else if ( tempdiff < 0) {
      newphase = 1;
    } else if ( tempdiff < 0.2) {
      newphase = 2;
    } else if ( tempdiff < 1 ) {
      newphase = 3;
    } else {
      newphase = 4;
    }

    printf("Controller/PreHeat: MT:%.2f RIMS:%.2f T:%.2f D:%.2f NP:%i\n",
	   mttemp, rimstemp, targettemp, tempdiff, newphase);

    if ( c_preheat_phase != newphase ) {
      c_preheat_phase = newphase;
      if ( newphase == 0 ) {
	setPIN("rimspump", PINState::Off);
	setPIN("rimsheat", PINState::Off);
      } else if ( newphase == 4 ) {
	setPIN("rimspump", PINState::On);
	setPIN("rimsheat", PINState::On);
      } else if ( newphase == 3 ) {
	setPIN("rimspump", PINState::On);
	setPIN("rimsheat", PINState::Pulsate, c_hecycletime, 0.7);
      } else if ( newphase == 2 ) {
	setPIN("rimspump", PINState::On);
	setPIN("rimsheat", PINState::Pulsate, c_hecycletime, 0.15);
      } else if ( newphase == 1 ) {
	setPIN("rimspump", PINState::On);
	setPIN("rimsheat", PINState::Off);
	c_ps.setState(ProcessState::States::NeedMalt);
	c_lastcontrol = 0;
      }
    }
#endif
    tempControl(targettemp, 6);
    if ( tempdiff < c_cfg->getTempAccuracy() ) {
	c_ps.setState(ProcessState::States::NeedMalt);
	c_lastcontrol = 0;
    }
  }

  void Controller::stageNeedMalt(PINTracker &_pt) {
    setPIN("rimsheat", PINState::Off);
    std::shared_ptr<PINTracker::PIN> buzzer(_pt.getPIN("buzzer"));

    // we keep on beeping
    if ( buzzer->getValue() != PINState::Pulsate ) {
      setPIN("buzzer", PINState::Pulsate, 2.1f, 0.4f);
    }

    // When the malts are added, temperature decreases
    // We have to heat it back up to the designated temperature
    float targettemp = c_prog->getStartTemp();
    tempControl(targettemp, c_cfg->getHeatOverhad());
  }

  void Controller::stageMashing(PINTracker &_pt) {
    printf("%s:%i:%s\n", __FILE__, __LINE__, __FUNCTION__);
  }

  void Controller::stageSparging(PINTracker &_pt) {
    printf("%s:%i:%s\n", __FILE__, __LINE__, __FUNCTION__);
  }

  void Controller::stagePreBoil(PINTracker &_pt) {
    printf("%s:%i:%s\n", __FILE__, __LINE__, __FUNCTION__);
  }

  void Controller::stageHopping(PINTracker &_pt) {
    printf("%s:%i:%s\n", __FILE__, __LINE__, __FUNCTION__);
  }

  void Controller::stageCooling(PINTracker &_pt) {
    printf("%s:%i:%s\n", __FILE__, __LINE__, __FUNCTION__);
  }

  void Controller::stageFinished(PINTracker &_pt) {
    printf("%s:%i:%s\n", __FILE__, __LINE__, __FUNCTION__);
  }


  void Controller::handleOutPIN(PINTracker::PIN &_pin) {
    c_mq_iocmd.send(PinStateMessage(_pin.getName(),
				    _pin.getNewValue(),
				    _pin.getNewCycletime(),
				    _pin.getNewOnratio()));
  }

  void Controller::tempControl(float _target, float _maxoverheat) {
    float mttemp = c_ps.getSensorTemp("MashTun");
    float rimstemp = c_ps.getSensorTemp("RIMS");
    float tgtdiff = _target - mttemp;
    float rimsdiff = rimstemp - _target;

    time_t now = time(0);

    if ( getPIN("rimspump")->getOldValue() != PINState::On )
      setPIN("rimspump", PINState::On);

    // only control every 3 seconds
    if ( now - c_lastcontrol < std::ceil(c_hecycletime) ) return;

    if ( tgtdiff < c_cfg->getTempAccuracy() ) {
      return;
    }
    c_lastcontrol = now;

    // coefficient based on the target temperature different
    float coeff_tgt=0;
    // coefficient based on the rims-mashtun tempdiff
    float coeff_rt=1;

    float halfpi = std::atan(INFINITY);
    if ( tgtdiff  > 0 ) coeff_tgt = std::atan(tgtdiff)/halfpi;
    if ( rimsdiff > 0 ) coeff_rt = std::atan(_maxoverheat-rimsdiff)/halfpi;
    if ( coeff_rt < 0 ) coeff_rt = 0;
    coeff_rt = std::pow(coeff_rt, 1.0/4);
    coeff_tgt = std::pow(coeff_tgt, 1.3);

    float heratio = coeff_tgt * coeff_rt;

#ifdef AEGIR_DEBUG
    printf("Controller::tempControl(): MT:%.2f RIMS:%.2f TGT:%.2f d_tgt:%.2f d_rims:%.2f c_tgt:%.2f c_rt:%.2f HEr:%.2f\n",
	   mttemp, rimstemp, _target, tgtdiff, rimsdiff, coeff_tgt, coeff_rt, heratio);
#endif

    auto pin_he = getPIN("rimsheat");
#ifdef AEGIR_DEBUG
    printf("Controller::tempControl(): rimsheat: ST:%hhu CT:%.2f OR:%.2f\n",
	   pin_he->getValue(), pin_he->getOldCycletime(), pin_he->getOldOnratio());
#endif

    if ( std::abs(pin_he->getOldOnratio()-heratio) > 0.03 )
      setPIN("rimsheat", PINState::Pulsate, c_hecycletime, heratio);
  }

  uint32_t Controller::calcHeatTime(uint32_t _vol, uint32_t _tempdiff, float _pkw) const {
    return (4.2 * _vol * _tempdiff)/_pkw;
  }
}
