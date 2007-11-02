/*
 * Oreka -- A media capture and retrieval platform
 * 
 * Copyright (C) 2005, orecx LLC
 *
 * http://www.orecx.com
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License.
 * Please refer to http://www.gnu.org/copyleft/gpl.html
 *
 */

#define _WINSOCKAPI_		// prevents the inclusion of winsock.h

#include <log4cxx/logger.h>
#include "CapturePort.h"
#include "Utils.h"
#include "ImmediateProcessing.h"
#include "Reporting.h"
#include "ConfigManager.h"

static LoggerPtr s_log;


CapturePort::CapturePort(CStdString& id)
{
	m_id = id;
	m_vadBelowThresholdSec = 0.0;
	m_vadUp = false;
	m_capturing = false;
	m_lastUpdated = 0;

	LoadFilters();
}

void CapturePort::LoadFilters()
{
	for(std::list<CStdString>::iterator it = CONFIG.m_capturePortFilters.begin(); it != CONFIG.m_capturePortFilters.end(); it++)
	{
		CStdString filterName = *it;
		FilterRef filter = FilterRegistry::instance()->GetNewFilter(filterName);
		if(filter.get())
		{
			m_filters.push_back(filter);
			LOG4CXX_DEBUG(s_log, CStdString("Adding filter:") + filterName);
		}
		else
		{
			LOG4CXX_ERROR(s_log, CStdString("Filter:") + filterName + " does not exist, please check <CapturePortFilters> in config.xml");
		}
	}
}


CStdString CapturePort::ToString()
{
	CStdString ret;
	return ret;
}

CStdString CapturePort::GetId()
{
	return m_id;
}

void CapturePort::FilterAudioChunk(AudioChunkRef& chunkRef)
{
	// Iterate through all filters
	std::list<FilterRef>::iterator it;
	for(it = m_filters.begin(); it != m_filters.end(); it++)
	{
		FilterRef filter = *it;
		filter->AudioChunkIn(chunkRef);
		filter->AudioChunkOut(chunkRef);
	}
}

void CapturePort::FilterCaptureEvent(CaptureEventRef& eventRef)
{
	// Iterate through all filters
	std::list<FilterRef>::iterator it;
	for(it = m_filters.begin(); it != m_filters.end(); it++)
	{
		FilterRef filter = *it;
		filter->CaptureEventIn(eventRef);
		filter->CaptureEventOut(eventRef);
	}
}

void CapturePort::AddAudioChunk(AudioChunkRef chunkRef)
{
	FilterAudioChunk(chunkRef);

	time_t now = time(NULL);
	m_lastUpdated = now;

	if(CONFIG.m_audioSegmentation)
	{
		if (m_audioTapeRef.get())
		{
			if ((now - m_audioTapeRef->m_beginDate) >= CONFIG.m_audioSegmentDuration)
			{
				// signal current tape stop event
				CaptureEventRef eventRef(new CaptureEvent);
				eventRef->m_type = CaptureEvent::EtStop;
				eventRef->m_timestamp = now;
				AddCaptureEvent(eventRef);

				// create new tape
				m_audioTapeRef.reset(new AudioTape(m_id));

				// signal new tape start event
				eventRef.reset(new CaptureEvent);
				eventRef->m_type = CaptureEvent::EtStart;
				eventRef->m_timestamp = now;
				AddCaptureEvent(eventRef);
			}
		}	
		else
		{
			// create new tape
			m_audioTapeRef.reset(new AudioTape(m_id));

			// signal new tape start event
			CaptureEventRef eventRef(new CaptureEvent);
			eventRef->m_type = CaptureEvent::EtStart;
			eventRef->m_timestamp = now;
			AddCaptureEvent(eventRef);
		}
	}
	else if (CONFIG.m_vad)
	{
		if(chunkRef->GetEncoding() == PcmAudio)
		{
			if(m_vadUp)
			{
				// There is an ongoing capture
				if (chunkRef->ComputeRmsDb() < CONFIG.m_vadLowThresholdDb)
				{
					// Level has gone below low threshold, increase holdon counter
					m_vadBelowThresholdSec += chunkRef->GetDurationSec();
				}
				else
				{
					// Level has gone above low threshold, reset holdon counter
					m_vadBelowThresholdSec = 0.0;
				}

				if (m_vadBelowThresholdSec > CONFIG.m_vadHoldOnSec)
				{
					// no activity detected for more than hold on time
					m_vadUp = false;

					// signal current tape stop event
					CaptureEventRef eventRef(new CaptureEvent);
					eventRef->m_type = CaptureEvent::EtStop;
					eventRef->m_timestamp = now;
					AddCaptureEvent(eventRef);
				}
			}
			else
			{
				// No capture is taking place yet
				if (chunkRef->ComputeRmsDb() > CONFIG.m_vadHighThresholdDb)
				{
					// Voice detected, start a new capture
					m_vadBelowThresholdSec = 0.0;
					m_vadUp = true;

					// create new tape
					m_audioTapeRef.reset(new AudioTape(m_id));

					// signal new tape start event
					CaptureEventRef eventRef(new CaptureEvent);
					eventRef->m_type = CaptureEvent::EtStart;
					eventRef->m_timestamp = now;
					AddCaptureEvent(eventRef);
				}
			}
		}
		else
		{
			LOG4CXX_ERROR(s_log, CStdString("Voice activity detection cannot be used on non PCM audio"));
		}
	}

	if (m_audioTapeRef.get() && m_capturing)
	{
		m_audioTapeRef->AddAudioChunk(chunkRef);

		// Signal to immediate processing thread that tape has new stuff
		ImmediateProcessing::GetInstance()->AddAudioTape(m_audioTapeRef);
	}
}

void CapturePort::AddCaptureEvent(CaptureEventRef eventRef)
{
	FilterCaptureEvent(eventRef);

	m_lastUpdated = time(NULL);

	AudioTapeRef audioTapeRef = m_audioTapeRef;

	// First of all, handle tape start
	if (eventRef->m_type == CaptureEvent::EtStart)
	{
		m_capturing = true;
		if (audioTapeRef.get())
		{
			audioTapeRef->SetShouldStop();	// force stop of previous tape
		}
		audioTapeRef.reset(new AudioTape(m_id));	// Create a new tape
		audioTapeRef->AddCaptureEvent(eventRef, true);

		m_audioTapeRef = audioTapeRef;
		LOG4CXX_INFO(s_log, "[" + m_audioTapeRef->m_trackingId + "] #" + m_id + " start");
	}

	if (!audioTapeRef.get())
	{
		LOG4CXX_WARN(s_log, "#" + m_id + ": received unexpected capture event:" 
			+ CaptureEvent::EventTypeToString(eventRef->m_type));
	}
	else
	{
		// Ok, at this point, we know we have a valid audio tape
		switch(eventRef->m_type)
		{
		case CaptureEvent::EtStart:
			break;
		case CaptureEvent::EtStop:

			m_capturing = false;
			LOG4CXX_INFO(s_log, "[" + audioTapeRef->m_trackingId + "] #" + m_id + " stop");
			audioTapeRef->AddCaptureEvent(eventRef, true);
			Reporting::Instance()->AddAudioTape(audioTapeRef);

			if (m_audioTapeRef->GetAudioFileRef().get())
			{
				// Notify immediate processing that tape has stopped
				ImmediateProcessing::GetInstance()->AddAudioTape(m_audioTapeRef);
			}
			else
			{
				// Received a stop but there is no valid audio file associated with the tape
				LOG4CXX_WARN(s_log, "[" + audioTapeRef->m_trackingId + "] #" + m_id + " no audio reported between last start and stop");
			}
			break;
		case CaptureEvent::EtEndMetadata:
			// Now that all metadata has been acquired, we can generate the tape start message
			Reporting::Instance()->AddAudioTape(audioTapeRef);
			break;
		case CaptureEvent::EtUpdate:
			audioTapeRef->AddCaptureEvent(eventRef, true);
			// Generate tape update message
			Reporting::Instance()->AddAudioTape(audioTapeRef);
			break;
		case CaptureEvent::EtDirection:
		case CaptureEvent::EtRemoteParty:
		case CaptureEvent::EtLocalParty:
		case CaptureEvent::EtLocalEntryPoint:
		default:
			audioTapeRef->AddCaptureEvent(eventRef, false);
		}
	}
}

bool CapturePort::IsExpired(time_t now)
{
	if((now - m_lastUpdated) > (10*60))	// 10 minutes
	{
		if(m_audioTapeRef.get())
		{
			if(m_audioTapeRef->m_state != AudioTape::StateActive)
			{
				return true;
			}
		}
		else
		{
			return true;
		}
	}
	return false;
}


//=======================================
CapturePorts::CapturePorts()
{
	m_ports.clear();
	m_lastHooveringTime = time(NULL);
	s_log = Logger::getLogger("port");
}

CapturePortRef CapturePorts::GetPort(CStdString & portId)
{
	Hoover();

	std::map<CStdString, CapturePortRef>::iterator pair;

	pair = m_ports.find(portId);

	if (pair == m_ports.end())
	{
		CapturePortRef nullPortRef;
		return nullPortRef;
	}
	else
	{
		return pair->second;
	}
}

CapturePortRef CapturePorts::AddAndReturnPort(CStdString & portId)
{
	//MutexGuard mutexGuard(m_mutex);		// To make sure a channel cannot be created twice - not used for now. CapturePorts only ever gets interaction from capture single thread 

	CapturePortRef portRef = GetPort(portId);
	if (portRef.get() == NULL)
	{
		// The port does not already exist, create it.
		CapturePortRef newPortRef(new CapturePort(portId));
		m_ports.insert(std::make_pair(portId, newPortRef));
		return newPortRef;
	}
	else
	{
		return portRef;
	}
}

void CapturePorts::Hoover()
{
	CStdString logMsg;
	time_t now = time(NULL);
	if( (now - m_lastHooveringTime) > 10)		// Hoover every 10 seconds
	{
		m_lastHooveringTime = now;
		int numPorts = m_ports.size();

		// Go round and detect inactive ports
		std::map<CStdString, CapturePortRef>::iterator pair;
		std::list<CapturePortRef> toDismiss;

		for(pair = m_ports.begin(); pair != m_ports.end(); pair++)
		{
			CapturePortRef port = pair->second;
			if(port->IsExpired(now))
			{
				toDismiss.push_back(port);
			}
		}

		// Discard inactive ports
		for (std::list<CapturePortRef>::iterator it = toDismiss.begin(); it != toDismiss.end() ; it++)
		{
			CapturePortRef port = *it;
			m_ports.erase(port->GetId());
			LOG4CXX_DEBUG(s_log,  port->GetId() + ": Expired");
		}
		logMsg.Format("Hoovered %d ports. New number:%d", (numPorts - m_ports.size()), m_ports.size());
		LOG4CXX_DEBUG(s_log,  logMsg);
	}
}


