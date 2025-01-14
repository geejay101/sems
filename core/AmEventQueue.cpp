/*
 * Copyright (C) 2002-2003 Fhg Fokus
 *
 * This file is part of SEMS, a free SIP media server.
 *
 * SEMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. This program is released under
 * the GPL with the additional exemption that compiling, linking,
 * and/or using OpenSSL is allowed.
 *
 * For a license to use the SEMS software under conditions
 * other than those described here, or to purchase support for this
 * software, please contact iptel.org by e-mail at the following addresses:
 *    info@iptel.org
 *
 * SEMS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "AmEventQueue.h"
#include "log.h"

#include <typeinfo>
#include "AmLcConfig.h"
AmEventQueue::AmEventQueue(AmEventHandler* handler)
  : handler(handler),
    wakeup_handler(NULL),
    ev_pending(false),
    finalized(false)
{
}

AmEventQueue::~AmEventQueue()
{
  m_queue.lock();
  while(!ev_queue.empty()){
    delete ev_queue.front();
    ev_queue.pop();
  }
  m_queue.unlock();
}

void AmEventQueue::postEvent(AmEvent* event)
{
  if (AmConfig.log_events) 
    DBG("AmEventQueue: trying to post event");

  m_queue.lock();

  if(event)
    ev_queue.push(event);

  if(!ev_pending.get()) {
    ev_pending.set(true);
    if (NULL != wakeup_handler)
      wakeup_handler->notify(this);
  }

  m_queue.unlock();

  if (AmConfig.log_events) 
    DBG("AmEventQueue: event posted");
}

void AmEventQueue::processEvents(EventStats *stats)
{
    timeval start, end, consumed_time;

    m_queue.lock();

    while(!ev_queue.empty()) {
        AmEvent* event = ev_queue.front();
        ev_queue.pop();
        m_queue.unlock();

        if(stats) {
            gettimeofday(&start, nullptr);
        }

        if (AmConfig.log_events)
            DBG("before processing event (%s)", typeid(*event).name());

        handler->process(event);

        if(stats) {
            gettimeofday(&end, nullptr);
            timersub(&end,&start,&consumed_time);
            stats->update(event, consumed_time);
        }

        if(AmConfig.log_events)
            DBG("event processed (%s)", typeid(*event).name());

        delete event;
        m_queue.lock();
    }

    ev_pending.set(false);
    m_queue.unlock();
}

void AmEventQueue::waitForEvent()
{
  ev_pending.wait_for();
}

void AmEventQueue::processSingleEvent()
{
  m_queue.lock();

  if (!ev_queue.empty()) {

    AmEvent* event = ev_queue.front();
    ev_queue.pop();
    m_queue.unlock();

    if (AmConfig.log_events) 
      DBG("before processing event");
    handler->process(event);
    if (AmConfig.log_events)
      DBG("event processed");
    delete event;

    m_queue.lock();
    if (ev_queue.empty())
      ev_pending.set(false);
  }

  m_queue.unlock();
}

bool AmEventQueue::eventPending() {
  m_queue.lock();
  bool res = !ev_queue.empty();
  m_queue.unlock();
  return res;
}

void AmEventQueue::setEventNotificationSink(AmEventNotificationSink* 
					    _wakeup_handler) {
  // locking actually not necessary - if replacing pointer is atomic 
  m_queue.lock(); 
  wakeup_handler = _wakeup_handler;
  if(wakeup_handler && ev_pending.get())
    wakeup_handler->notify(this);
  m_queue.unlock();
}
