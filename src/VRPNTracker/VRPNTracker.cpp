/*
 * Ubitrack - Library for Ubiquitous Tracking
 * Copyright 2006, Technische Universitaet Muenchen, and individual
 * contributors as indicated by the @authors tag. See the 
 * copyright.txt in the distribution for a full listing of individual
 * contributors.
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this software; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA, or see the FSF site: http://www.fsf.org.
 */


#include "vrpn_Connection.h" // Missing this file?  Get the latest VRPN distro at
#include "vrpn_Tracker.h"    //    ftp://ftp.cs.unc.edu/pub/packages/GRIP/vrpn
#include "vrpn_Analog.h"

#ifdef WIN32
#include <utUtil/CleanWindows.h>
#ifndef _WIN32_WINNT
	#define _WIN32_WINNT WINVER
#endif

#include <objbase.h>
#include <atlbase.h>
#include <windows.h>
#include <assert.h>

#endif //WIN32

#include <map>
#include <string>
#include <deque>
#include <cstdlib>
#include <boost/thread.hpp>
#include <boost/thread/xtime.hpp>
#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <boost/ref.hpp>
#include <boost/scoped_ptr.hpp>
#include <log4cpp/Category.hh>

#include <utDataflow/PushSupplier.h>
#include <utDataflow/ComponentFactory.h>
#include <utDataflow/Module.h>
#include <utDataflow/Component.h>
#include <utMeasurement/Measurement.h>
#include <utMeasurement/TimestampSync.h>
#include <utUtil/Exception.h>
#include <utUtil/OS.h>


static log4cpp::Category& logger( log4cpp::Category::getInstance( "UbitrackMVL.VRPNTracker" ) );

using namespace Ubitrack;
using namespace Ubitrack::Dataflow;

namespace Ubitrack { namespace Drivers {


// forward declaration
class VRPNModuleComponent;

MAKE_NODEATTRIBUTEKEY_DEFAULT( VRPNModuleKey, std::string, "VRPN", "hostPort", "localhost:3883" );


/**
 * Component key for VRPNTracker.
 * Represents the body name
 */
class VRPNModuleComponentKey
{
public:
	// still ugly refactor vrpntracker driver sometime..
	// construct from configuration
	VRPNModuleComponentKey( boost::shared_ptr< Graph::UTQLSubgraph > subgraph )
	: m_body( "" )
    , m_channelId(0)
    , m_targetClass(0)
	{
		Graph::UTQLSubgraph::EdgePtr config;

	  if ( subgraph->hasEdge( "VRPNToTarget" ) )
		  config = subgraph->getEdge( "VRPNToTarget" );

	  if ( !config )
	  {
		  UBITRACK_THROW( "VRPNTracker Pattern has no \"VRPNToTarget\" edge");
	  }

	  config->getAttributeData( "vrpnBodyId", m_body );
	  if ( m_body == "" )
            UBITRACK_THROW( "Missing or invalid \"vrpnBodyId\" attribute on \"VRPNToTarget\" edge" );
	  
	  std::string& dfgclass = subgraph->m_DataflowClass;
    if (dfgclass == "VRPNTracker")
    {
		  m_targetClass = 1;
      config->getAttributeData("ChannelId", m_channelId);
	  };
	  if (dfgclass == "VRPNAnalog"){
		  m_targetClass = 2;
      config->getAttributeData("ChannelId", m_channelId);
	  };
	  if (dfgclass == "VRPNAnalogList"){
		  m_targetClass = 3;
	  };
    if (dfgclass == "VRPNTrackerList")
    {
      m_targetClass = 4;
    }

	}

	// construct from body number
	VRPNModuleComponentKey( std::string n, int targetClass, int channelId )
		: m_body( n )
		, m_targetClass(targetClass)
    , m_channelId(channelId)
 	{}

	int getTargetClass() const
	{
		return m_targetClass;
	}

	std::string getBody() const
	{
		return m_body;
	}

  int getChannelId() const
  {
    return m_channelId;
  }

	// less than operator for map
	bool operator<( const VRPNModuleComponentKey& b ) const
    {
      if (m_targetClass == b.m_targetClass)
      {
			int r = m_body.compare(b.m_body);
        if (r == 0)
        {
          return (m_channelId < b.m_channelId);
        }
        else
        {
			if (r < 0)
				return true;
			return false;
		}

      }
		return (m_targetClass < b.m_targetClass);
    }

protected:
	std::string m_body;
	int m_targetClass;
  int m_channelId;
};








/**
 * Module for VRPNtracker.
 * Does all the work
 */

class VRPNModule
	: public Module<VRPNModuleKey, VRPNModuleComponentKey, VRPNModule, VRPNModuleComponent>
{
public:
	/** constructor, creates thread */
	VRPNModule( const VRPNModuleKey& key, boost::shared_ptr< Graph::UTQLSubgraph > pCfg, FactoryHelper* pFactory )
		: Module<VRPNModuleKey, VRPNModuleComponentKey, VRPNModule, VRPNModuleComponent>( key, pFactory ),
		m_bStop( false ),
		m_nTrackers( 0 )	{

	}

	/** destructor, stops thread */
	~VRPNModule()
	{
		
	}


	virtual void startModule() {
		LOG4CPP_INFO( logger, "VRPNTracker starting, connection: " << m_moduleKey.get());	
		connection = vrpn_get_connection_by_name(m_moduleKey.get().c_str());
		m_Thread.reset( new boost::thread( boost::bind( &VRPNModule::threadProc, this ) ) );

		ComponentList l = this->getAllComponents();
		for (ComponentList::iterator it = l.begin(); it != l.end(); it++) {
			boost::mutex::scoped_lock l( m_queueMutex );
			m_queue.push_back( boost::bind( &VRPNModule::myCreateTracker, this, (*it) ) );
			m_queueCondition.notify_all();
		}
	
    m_bStop = false;
	}

	virtual void stopModule() {
		LOG4CPP_INFO( logger, "VRPNTracker stopping");	

		ComponentList l = this->getAllComponents();
		for (ComponentList::iterator it = l.begin(); it != l.end(); it++) {
			boost::mutex::scoped_lock l( m_queueMutex );
			m_queue.push_back( boost::bind( &VRPNModule::myDestroyTracker, this, *it ) );
			m_queueCondition.notify_all();
		}

		m_bStop = true;
		m_Thread->join();
		Module<VRPNModuleKey, VRPNModuleComponentKey, VRPNModule, VRPNModuleComponent>::stopModule();
	}

	virtual boost::shared_ptr<VRPNModuleComponent> createComponent(const std::string& type, const std::string& name, boost::shared_ptr< Graph::UTQLSubgraph> subgraph,
		const ComponentKey& key, ModuleClass* pModule);


protected:
	// thread main loop
	void threadProc();

	void myCreateTracker( boost::shared_ptr<VRPNModuleComponent> c );
	void myDestroyTracker( boost::shared_ptr<VRPNModuleComponent> c );

	// the thread
	boost::scoped_ptr< boost::thread > m_Thread;

	// stop the thread?
	volatile bool m_bStop;

	// number of cams open
	int m_nTrackers;

	// a mutex object to synchronize cam calls
	boost::mutex m_queueMutex;

	// notification when the queue has changed
	boost::condition m_queueCondition;

	typedef std::deque< boost::function< void() > > QueueType;
	QueueType m_queue;

	vrpn_Connection *connection;

};


/**
 * Component for VRPNTracker.
 * 
 */

class VRPNModuleComponent
	: public VRPNModule::Component
{

public:


	/** constructor */
	VRPNModuleComponent(const std::string& sName, boost::shared_ptr< Graph::UTQLSubgraph > pCfg, const VRPNModuleComponentKey& componentKey, VRPNModule* pModule) :
		VRPNModule::Component(sName, componentKey, pModule)
	{
	}

	/** destructor */
	~VRPNModuleComponent() {};


	virtual bool init(vrpn_Connection* conn) {
		return true;
	};

	virtual bool exit(vrpn_Connection* conn) {
		return true;
	};

protected:
	


};

class VRPN6DTrackerComponent
	: public VRPNModuleComponent
{

public:
	static void	VRPN_CALLBACK handle_pos (void *userdata, const vrpn_TRACKERCB t)
	{
		VRPN6DTrackerComponent* vt = (VRPN6DTrackerComponent*)userdata;
    if (t.sensor != vt->m_channelId) return;

		Measurement::Timestamp ts = Measurement::now();
		Math::Pose mathPose = Math::Pose(Math::Quaternion(t.quat[0],t.quat[1],t.quat[2],t.quat[3]), Math::Vector<double, 3 >(t.pos));
		Measurement::Pose result = Measurement::Pose(ts, mathPose);
		vt->m_output.send(result);
	}

	/** constructor */
	VRPN6DTrackerComponent(const std::string& sName, boost::shared_ptr< Graph::UTQLSubgraph > pCfg, const VRPNModuleComponentKey& componentKey, VRPNModule* pModule) :
		VRPNModuleComponent(sName, pCfg,  componentKey, pModule)
	, m_output("VRPNToTarget", *this)	
	, tracker(NULL)
	{
    Graph::UTQLSubgraph::EdgePtr config;
    if (pCfg->hasEdge("VRPNToTarget"))
      config = pCfg->getEdge("VRPNToTarget");

    if (!config)
    {
      UBITRACK_THROW("VRPNTracker Pattern has no \"VRPNToTarget\" edge");
    }

    config->getAttributeData("ChannelId", m_channelId);
	}

	/** destructor */
	~VRPN6DTrackerComponent() {};


	virtual bool init(vrpn_Connection* conn) {
		LOG4CPP_INFO( logger, "VRPNTracker connect Tracker: " << m_componentKey.getBody());	
		tracker = new vrpn_Tracker_Remote(m_componentKey.getBody().c_str(), conn);
	  	tracker->register_change_handler((void *)this, &handle_pos);
		return true;
	}
	
	virtual bool exit(vrpn_Connection* conn) {
		LOG4CPP_INFO( logger, "VRPNTracker disconnect Tracker: " << m_componentKey.getBody());	
	  	tracker->unregister_change_handler((void *)this, &handle_pos);
		return true;
	}

protected:
	vrpn_Tracker_Remote *tracker;
  int m_channelId;
	Dataflow::PushSupplier< Measurement::Pose > m_output;

};

class VRPN6DTrackerListComponent
  : public VRPNModuleComponent
{

public:
  static void	VRPN_CALLBACK handle_pos(void *userdata, const vrpn_TRACKERCB t)
  {
    VRPN6DTrackerListComponent* vt = (VRPN6DTrackerListComponent*)userdata;
    Measurement::Timestamp ts = Measurement::now();
    Math::Pose mathPose = Math::Pose(Math::Quaternion(t.quat[0], t.quat[1], t.quat[2], t.quat[3]), Math::Vector<double, 3 >(t.pos));
    if (t.sensor < vt->m_PoseList.size())
    {
      vt->m_PoseList[t.sensor] = mathPose;
    }
    Measurement::PoseList poseList(ts, vt->m_PoseList);
    vt->m_output.send(poseList);
  }

  /** constructor */
  VRPN6DTrackerListComponent(const std::string& sName, boost::shared_ptr< Graph::UTQLSubgraph > pCfg, const VRPNModuleComponentKey& componentKey, VRPNModule* pModule) :
    VRPNModuleComponent(sName, pCfg, componentKey, pModule)
    , m_output("VRPNToTarget", *this)
    , m_numSensors(1)
    , tracker(NULL)
  {
    Graph::UTQLSubgraph::EdgePtr config;
    if (pCfg->hasEdge("VRPNToTarget"))
      config = pCfg->getEdge("VRPNToTarget");

    if (!config)
    {
      UBITRACK_THROW("VRPNTracker Pattern has no \"VRPNToTarget\" edge");
    }

    config->getAttributeData("NumSensors", m_numSensors);

    m_PoseList.resize(m_numSensors);
  }

  /** destructor */
  ~VRPN6DTrackerListComponent() {};


  virtual bool init(vrpn_Connection* conn) {
    LOG4CPP_INFO(logger, "VRPNTracker connect Tracker: " << m_componentKey.getBody());
    tracker = new vrpn_Tracker_Remote(m_componentKey.getBody().c_str(), conn);
    tracker->register_change_handler((void *)this, &handle_pos);
    return true;
  }

  virtual bool exit(vrpn_Connection* conn) {
    LOG4CPP_INFO(logger, "VRPNTracker disconnect Tracker: " << m_componentKey.getBody());
    tracker->unregister_change_handler((void *)this, &handle_pos);
    return true;
  }

protected:
  vrpn_Tracker_Remote *tracker;
  int m_numSensors;
  std::vector<Math::Pose> m_PoseList;
  Dataflow::PushSupplier< Measurement::PoseList > m_output;

};

class VRPNAnalogComponent
	: public VRPNModuleComponent
{

public:

	static void	VRPN_CALLBACK handle_pos(void *userdata, const vrpn_ANALOGCB t)
	{
		VRPNAnalogComponent* vt = (VRPNAnalogComponent*)userdata;
		Measurement::Timestamp ts = Measurement::now();
		Math::Scalar<double> mathDistance = Math::Scalar<double>(t.channel[vt->m_channelId]);
		Measurement::Distance result = Measurement::Distance(ts, mathDistance);
		vt->m_output.send(result);
	}

	/** constructor */
	VRPNAnalogComponent(const std::string& sName, boost::shared_ptr< Graph::UTQLSubgraph > pCfg, const VRPNModuleComponentKey& componentKey, VRPNModule* pModule) :
		VRPNModuleComponent(sName, pCfg, componentKey, pModule)
		, m_output("VRPNToTarget", *this)
		, tracker(NULL)
	{
		Graph::UTQLSubgraph::EdgePtr config;
		if (pCfg->hasEdge("VRPNToTarget"))
			config = pCfg->getEdge("VRPNToTarget");

		if (!config)
		{
			UBITRACK_THROW("VRPNTracker Pattern has no \"VRPNToTarget\" edge");
		}

		config->getAttributeData("ChannelId", m_channelId);

	}

	/** destructor */
	~VRPNAnalogComponent() {};


	virtual bool init(vrpn_Connection* conn) {
		LOG4CPP_INFO(logger, "VRPNTracker connect Tracker: " << m_componentKey.getBody());
		tracker = new vrpn_Analog_Remote(m_componentKey.getBody().c_str(), conn);
		tracker->register_change_handler((void *)this, &handle_pos);
		return true;
	}

	virtual bool exit(vrpn_Connection* conn) {
		LOG4CPP_INFO(logger, "VRPNTracker disconnect Tracker: " << m_componentKey.getBody());
		tracker->unregister_change_handler((void *)this, &handle_pos);
		return true;
	}

protected:
	vrpn_Analog_Remote *tracker;
	int m_channelId;
	Dataflow::PushSupplier< Measurement::Distance > m_output;

};


class VRPNAnalogListComponent
	: public VRPNModuleComponent
{

public:

	static void	VRPN_CALLBACK handle_pos(void *userdata, const vrpn_ANALOGCB t)
	{
		VRPNAnalogListComponent* vt = (VRPNAnalogListComponent*)userdata;
		Measurement::Timestamp ts = Measurement::now();
		int num_channel = t.num_channel;
		std::vector<Math::Scalar<double> > mathDistanceList(num_channel);
		for (int i = 0; i < num_channel;i++){
			mathDistanceList[i] = Math::Scalar<double>(t.channel[i]);
		}	
		Measurement::DistanceList result = Measurement::DistanceList(ts, mathDistanceList);
		vt->m_output.send(result);
	}

	/** constructor */
	VRPNAnalogListComponent(const std::string& sName, boost::shared_ptr< Graph::UTQLSubgraph > pCfg, const VRPNModuleComponentKey& componentKey, VRPNModule* pModule) :
		VRPNModuleComponent(sName, pCfg, componentKey, pModule)
		, m_output("VRPNToTarget", *this)
		, tracker(NULL)
	{
		

	}

	/** destructor */
	~VRPNAnalogListComponent() {};


	virtual bool init(vrpn_Connection* conn) {
		LOG4CPP_INFO(logger, "VRPNTracker connect Tracker: " << m_componentKey.getBody());
		tracker = new vrpn_Analog_Remote(m_componentKey.getBody().c_str(), conn);
		tracker->register_change_handler((void *)this, &handle_pos);
		return true;
	}

	virtual bool exit(vrpn_Connection* conn) {
		LOG4CPP_INFO(logger, "VRPNTracker disconnect Tracker: " << m_componentKey.getBody());
		tracker->unregister_change_handler((void *)this, &handle_pos);
		return true;
	}

protected:
	vrpn_Analog_Remote *tracker;
	Dataflow::PushSupplier< Measurement::DistanceList > m_output;

};



void VRPNModule::threadProc()
{
	while ( !m_bStop || m_nTrackers > 0 )
	{
		// get next event from queue
		boost::function< void() > nextCall;
		{
			boost::mutex::scoped_lock l( m_queueMutex );
			if ( m_queue.empty() && m_nTrackers )
			{	

#ifdef WIN32
				// let the message dispatching do something
				MSG msg;
				if( PeekMessage( &msg, NULL, 0, 0, PM_NOREMOVE ) )
				{
					if(GetMessage( &msg, NULL, 0, 0 ) )
					{
						TranslateMessage(&msg);
						DispatchMessage(&msg);
					}
				}
#endif //WIN32
				//no new events, now handle trackers
				boost::this_thread::sleep(boost::posix_time::milliseconds(5));
				
				//for ( ComponentList::iterator it = comps.begin(); it != comps.end(); it++ )
				//(*it)->poll();

				connection->mainloop();

			}

			if ( !m_queue.empty() )
			{
				nextCall = m_queue.front();
				m_queue.pop_front();
			}
		}

		// execute the queued event
		if ( nextCall )
			nextCall();		
		
	}
}
void VRPNModule::myCreateTracker( boost::shared_ptr<VRPNModuleComponent> c ) {
	bool b;
	b = c->init(connection);
	if( b )
		m_nTrackers++;
}	

void VRPNModule::myDestroyTracker( boost::shared_ptr<VRPNModuleComponent> c ) {
	bool b;
	b = c->exit(connection);
	if( b )
		m_nTrackers--;
}


 boost::shared_ptr<VRPNModuleComponent> VRPNModule::createComponent(const std::string& type, const std::string& name, boost::shared_ptr< Graph::UTQLSubgraph> subgraph,
	const ComponentKey& key, ModuleClass* pModule){
	if (type == "VRPNTracker"){
		return boost::shared_ptr<VRPNModuleComponent>(new VRPN6DTrackerComponent(name, subgraph, key, pModule));
	}
  else if (type == "VRPNTrackerList"){
    return boost::shared_ptr<VRPNModuleComponent>(new VRPN6DTrackerListComponent(name, subgraph, key, pModule));
	}
	else if (type == "VRPNAnalog"){
		return boost::shared_ptr<VRPNModuleComponent>(new VRPNAnalogComponent(name, subgraph, key, pModule));
	}
	else if (type == "VRPNAnalogList"){
		return boost::shared_ptr<VRPNModuleComponent>(new VRPNAnalogListComponent(name, subgraph, key, pModule));
	}
	UBITRACK_THROW("Class " + type + " not supported by vrpn module");
}
	
} } // namespace Ubitrack::Driver

UBITRACK_REGISTER_COMPONENT( Dataflow::ComponentFactory* const cf ) {
  std::vector<std::string> vrpnComponents = { "VRPNTracker", "VRPNTrackerList", "VRPNAnalog", "VRPNAnalogList" };
  cf->registerModule< Ubitrack::Drivers::VRPNModule >(vrpnComponents);
}
