/*
 * Copyright (C) 2006 iptego GmbH
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

#include "SIPRegistrarClient.h"
#include "AmUtils.h"
#include "AmPlugIn.h"
#include "AmSessionContainer.h"
#include "AmEventDispatcher.h"
#include "sip/parse_via.h"

#define MOD_NAME "registrar_client"

#include <unistd.h>

#define CFG_OPT_NAME_SHAPER_MIN_INTERVAL "min_interval_per_domain_msec"
#define CFG_OPT_NAME_DEFAULT_EXPIRES "default_expires"
#define CFG_OPT_NAME_EXPORT_METRICS "export_metrics"

#define DEFAULT_EXPIRES 1800

#define TIMEOUT_CHECKING_INTERVAL 200000 //microseconds
#define EPOLL_MAX_EVENTS    2048

EXPORT_PLUGIN_CLASS_FACTORY(SIPRegistrarClient);
EXPORT_PLUGIN_CONF_FACTORY(SIPRegistrarClient);

static void reg2arg(const map<string, AmSIPRegistration*>::iterator &it, AmArg &ret, const RegShaper::timep &now) {
    AmArg r;
    AmSIPRegistration *reg = it->second;
    const SIPRegistrationInfo &ri = reg->getInfo();
    AmSIPRegistration::RegistrationState state;

    if(reg->getUnregistering())
        return; //hide unregistering registrations

    state = reg->getState();

    r["handle"] = it->first;
    r["id"] = ri.id;
    r["domain"] = ri.domain;
    r["user"] = ri.user;
    r["display_name"] = ri.name;
    r["auth_user"] = ri.auth_user;
    r["proxy"] = ri.proxy;
    r["contact"] = ri.contact;
    r["expires_interval"] = ri.expires_interval;
    r["expires"] =   (int)reg->reg_expires;
    r["force_reregister"] = ri.force_expires_interval;
    r["retry_delay"] = ri.retry_delay;
    r["max_attempts"] = ri.max_attempts;
    r["attempt"] = ri.attempt;
    r["transport_protocol_id"] = ri.transport_protocol_id;
    r["proxy_transport_protocol_id"] = ri.proxy_transport_protocol_id;
    r["event_sink"] = reg->getEventSink();
    r["last_request_time"] = (int)reg->reg_send_begin;
    r["last_succ_reg_time"] = (int)reg->reg_begin;
    r["expires_left"] = (int)reg->getExpiresLeft();
    r["state_code"] = state;
    r["state"] = getSIPRegistationStateString(state);
    r["last_request_contact"] = reg->request_contact;
    r["last_reply_contacts"] = reg->reply_contacts;
    if(reg->error_code!=0) {
        r["last_error_code"] = reg->error_code;
        r["last_error_reason"] = reg->error_reason;
        r["last_error_initiator"] = getSIPRegistationErrorInitiatorString(reg->error_initiatior);
    } else {
        r["last_error_code"] = 0;
        r["last_error_reason"] = AmArg();
        r["last_error_initiator"] = AmArg();
    }
    if(reg->postponed) {
        r["postpone_timeout_msec"] =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                    reg->postponed_next_attempt-now).count();
    } else {
        r["postpone_timeout_msec"] = 0;
    }
    r["resolve_priority"] = ri.resolve_priority;
    r["scheme_id"] = ri.scheme_id;
    r["sip_interface_name"] = ri.sip_interface_name;
    ret.push(r);
}

//-----------------------------------------------------------
SIPRegistrarClient* SIPRegistrarClient::_instance=0;

SIPRegistrarClient* SIPRegistrarClient::instance()
{
    if(_instance == NULL){
        _instance = new SIPRegistrarClient(MOD_NAME);
    }
    return _instance;
}

SIPRegistrarClient::SIPRegistrarClient(const string& name)
  : AmDynInvokeFactory(MOD_NAME),
    AmConfigFactory(MOD_NAME),
    AmEventFdQueue(this),
    stopped(false),
    default_expires(DEFAULT_EXPIRES),
    uac_auth_i(NULL)
{ }

int SIPRegistrarClient::configure(const std::string& config)
{
    cfg_opt_t opt[] = {
        CFG_INT(CFG_OPT_NAME_SHAPER_MIN_INTERVAL, 0, CFGF_NODEFAULT),
        CFG_INT(CFG_OPT_NAME_DEFAULT_EXPIRES, DEFAULT_EXPIRES, CFGF_NONE),
        CFG_BOOL(CFG_OPT_NAME_EXPORT_METRICS, cfg_false, CFGF_NONE),
        CFG_END()
    };
    cfg_t *cfg = cfg_init(opt, CFGF_NONE);
    if(!cfg) return -1;
    switch(cfg_parse_buf(cfg, config.c_str())) {
    case CFG_SUCCESS:
        break;
    case CFG_PARSE_ERROR:
        ERROR("configuration of module %s parse error",MOD_NAME);
        cfg_free(cfg);
        return -1;
    default:
        ERROR("unexpected error on configuration of module %s processing",MOD_NAME);
        cfg_free(cfg);
        return -1;
    }

    if(cfg_size(cfg, CFG_OPT_NAME_SHAPER_MIN_INTERVAL)) {
        int i = cfg_getint(cfg, CFG_OPT_NAME_SHAPER_MIN_INTERVAL);
        if(i) {
            DBG("set shaper min interval to %dmsec",i);
            if(i < (TIMEOUT_CHECKING_INTERVAL/1000)) {
                WARN("shaper min interval %dmsec is less than timer interval %dmsec. "
                     "set it to timer interval",
                     i,(TIMEOUT_CHECKING_INTERVAL/1000));
                i = TIMEOUT_CHECKING_INTERVAL/1000;
            }
            shaper.set_min_interval(i);
        }
    }
    default_expires = cfg_getint(cfg, CFG_OPT_NAME_DEFAULT_EXPIRES);
    if(cfg_true==cfg_getbool(cfg,CFG_OPT_NAME_EXPORT_METRICS))
        statistics::instance()->add_groups_container("registrar_client", this, false);

    cfg_free(cfg);
    return 0;
}

int SIPRegistrarClient::reconfigure(const std::string& config)
{
    return configure(config);
}

void SIPRegistrarClient::run()
{
    int ret;
    bool running;
    struct epoll_event events[EPOLL_MAX_EVENTS];

    setThreadName("sip-reg-client");

    DBG("SIPRegistrarClient starting...");

    AmDynInvokeFactory* uac_auth_f = AmPlugIn::instance()->getFactory4Di("uac_auth");
    if (uac_auth_f == NULL) {
        DBG("unable to get a uac_auth factory. registrations will not be authenticated.");
        DBG("(do you want to load uac_auth module?)");
    } else {
        uac_auth_i = uac_auth_f->getInstance();
    }

    /*
    while (!stop_requested.get()) {
        if (registrations.size()) {
            unsigned int cnt = 250;
            while (cnt > 0) {
                usleep(2000); // every 2 ms
                if(stop_requested.get())
                    break;
                processEvents();
                cnt--;
            }
            checkTimeouts();
        } else {
            waitForEvent();
            processEvents();
        }
    }*/

    running = true;
    do {
        ret = epoll_wait(epoll_fd, events, EPOLL_MAX_EVENTS, -1);

        if(ret == -1 && errno != EINTR) {
            ERROR("epoll_wait: %s",strerror(errno));
        }

        if(ret < 1)
            continue;

        for (int n = 0; n < ret; ++n) {
            struct epoll_event &e = events[n];
            int f = e.data.fd;

            if(!(e.events & EPOLLIN)){
                continue;
            }

            if(f==timer){
                checkTimeouts();
                timer.read();
            } else if(f== -queue_fd()){
                clear_pending();
                processEvents();
            } else if(f==stop_event){
                stop_event.read();
                running = false;
                break;
            }
        }
    } while(running);

    AmEventDispatcher::instance()->delEventQueue(REG_CLIENT_QUEUE);
    epoll_unlink(epoll_fd);
    close(epoll_fd);

    onServerShutdown();
    stopped.set(true);
}

void SIPRegistrarClient::checkTimeouts()
{
    struct timeval now;
    gettimeofday(&now, NULL);
    RegShaper::timep now_point(std::chrono::system_clock::now());
    reg_mut.lock();
    vector<string> remove_regs;

    for (map<string, AmSIPRegistration*>::iterator it = registrations.begin();
        it != registrations.end(); it++)
    {
        AmSIPRegistration* reg = it->second;
        if (reg->postponed) {
            if(reg->postponingExpired(now_point)) {
                reg->onPostponeExpired();
            }
        } else if (reg->active) {
            if (reg->registerExpired(now.tv_sec)) {
                reg->onRegisterExpired();
            } else if (!reg->waiting_result &&
                       reg->timeToReregister(now.tv_sec))
            {
                reg->doRegistration();
            }
        } else if (reg->remove) {
            remove_regs.push_back(it->first);
        } else if (!reg->waiting_result && reg->error_code!=0 &&
                   reg->registerSendTimeout(now.tv_sec))
        {
            reg->onRegisterSendTimeout();
        }
    }

    for (vector<string>::iterator it = remove_regs.begin();
         it != remove_regs.end(); it++)
    {
        AmSIPRegistration *reg = remove_reg_unsafe(*it);
        if (reg)
            delete reg;
    }

    reg_mut.unlock();
}

int SIPRegistrarClient::onLoad()
{
    if((epoll_fd = epoll_create(3)) == -1){
        ERROR("epoll_create call failed");
        return false;
    }

    epoll_link(epoll_fd);
    stop_event.link(epoll_fd);

    timer.set(TIMEOUT_CHECKING_INTERVAL);
    timer.link(epoll_fd);

    AmEventDispatcher::instance()->addEventQueue(REG_CLIENT_QUEUE, this);

    instance()->start();

    return 0;
}

void SIPRegistrarClient::onServerShutdown()
{
    // TODO: properly wait until unregistered, with timeout
    DBG("shutdown SIP registrar client: deregistering");
    for (std::map<std::string, AmSIPRegistration*>::iterator it=
         registrations.begin(); it != registrations.end(); it++)
    {
        it->second->doUnregister();
        delete it->second;
        AmEventDispatcher::instance()->delEventQueue(it->first);
    }
}

void SIPRegistrarClient::process(AmEvent* ev) 
{
    if (ev->event_id == E_SYSTEM) {
        AmSystemEvent* sys_ev = dynamic_cast<AmSystemEvent*>(ev);
        if(sys_ev){
            DBG("Session received system Event");
            if (sys_ev->sys_event == AmSystemEvent::ServerShutdown) {
                stop_event.fire();
            }
            return;
        }
    }

    AmSipReplyEvent* sip_rep = dynamic_cast<AmSipReplyEvent*>(ev);
    if (sip_rep) {
        onSipReplyEvent(sip_rep);
        return;
    }

    SIPNewRegistrationEvent* new_reg = dynamic_cast<SIPNewRegistrationEvent*>(ev);
    if (new_reg) {
        onNewRegistration(new_reg);
        return;
    }

    SIPRemoveRegistrationEvent* rem_reg = dynamic_cast<SIPRemoveRegistrationEvent*>(ev);
    if (rem_reg) {
        onRemoveRegistration(rem_reg);
        return;
    }

    BusReplyEvent *bus_event = dynamic_cast<BusReplyEvent *>(ev);
    if(bus_event) {
        onBusEvent(bus_event);
        return;
    }

    DBG("got unknown event. ignore");
}

void SIPRegistrarClient::onSipReplyEvent(AmSipReplyEvent* ev)
{
    AmSIPRegistration* reg = get_reg(ev->reply.from_tag);
    if (reg != NULL) {
        reg->getDlg()->onRxReply(ev->reply);
    }
}

void SIPRegistrarClient::onNewRegistration(SIPNewRegistrationEvent* new_reg)
{
    AmSIPRegistration* reg =
        new AmSIPRegistration(new_reg->handle,
                              new_reg->info,
                              new_reg->sess_link,
                              shaper);

    if (uac_auth_i != NULL) {
        DBG("enabling UAC Auth for new registration.");
        // get a sessionEventHandler from uac_auth
        AmArg di_args,ret;
        AmArg a;
        a.setBorrowedPointer(reg);
        di_args.push(a);
        di_args.push(a);

        uac_auth_i->invoke("getHandler", di_args, ret);
        if (!ret.size()) {
            ERROR("Can not add auth handler to new registration!");
        } else {
            AmObject* p = ret.get(0).asObject();
            if (p != NULL) {
                AmSessionEventHandler* h = dynamic_cast<AmSessionEventHandler*>(p);
                if (h != NULL)
                    reg->setSessionEventHandler(h);
            }
        }
    }

    if(new_reg->info.expires_interval!=0)
        reg->setExpiresInterval(new_reg->info.expires_interval);
    else
        reg->setExpiresInterval(default_expires);

    if(new_reg->info.force_expires_interval)
        reg->setForceExpiresInterval(true);

    if(!add_reg(new_reg->handle, reg))
        return;

    reg->doRegistration();
}

void SIPRegistrarClient::onRemoveRegistration(SIPRemoveRegistrationEvent* reg)
{
    reg_mut.lock();

    RegHash::iterator it;

    if(reg->is_id) {
        RegHash::iterator id_it = registrations_by_id.find(reg->handle_or_id);
        if(id_it==registrations_by_id.end()) {
            reg_mut.unlock();
            DBG("onRemoveRegistration: remove event with not existent id: %s",
                reg->handle_or_id.c_str());
            return;
        }
        it = registrations.find(id_it->second->getHandle());
        if(it==registrations.end()) {
            ERROR("onRemoveRegistration: inconsistence. "
                  "handle %s by id %s is not exist in hash by handlers. "
                  "remove it from registrations_by_id hash",
                  id_it->second->getHandle().c_str(),
                  reg->handle_or_id.c_str());
            registrations_by_id.erase(id_it);
            reg_mut.unlock();
            return;
        }
    } else {
        it = registrations.find(reg->handle_or_id);
        if(it==registrations.end()) {
            reg_mut.unlock();
            DBG("onRemoveRegistration: remove event with not existent handle: %s",
                reg->handle_or_id.c_str());
            return;
        }
    }

    AmSIPRegistration *_reg = it->second;

    registrations_by_id.erase(_reg->getInfo().id);

    reg_mut.unlock();

    _reg->doUnregister();
}

void SIPRegistrarClient::processAmArgRegistration(AmArg &data)
{
#define DEF_AND_VALIDATE_OPTIONAL_STR(key) \
    string key; \
    if(data.hasMember(#key)) { \
        AmArg & key ## _arg = data[#key]; \
        if(!isArgCStr(key ## _arg)) { ERROR("unexpected '" #key "' type. expected string"); return; } \
        key = key ## _arg.asCStr(); \
    }

    if(!isArgStruct(data)) { ERROR("unexpected payload type in BusReplyEvent"); return; }

    string action;
    if(!data.hasMember("action")) { ERROR("missed 'action' in BusReplyEvent payload"); return; }
    AmArg & action_arg = data["action"];
    if(!isArgCStr(action_arg)) { ERROR("unexpected 'action' type. expected string"); return; } \
    action = action_arg.asCStr();

    if(action=="create") {
        SIPRegistrationInfo info;
        if(!info.init_from_amarg(data)) return;
        DEF_AND_VALIDATE_OPTIONAL_STR(sess_link);
        DEF_AND_VALIDATE_OPTIONAL_STR(handle);

        SIPRegistrarClient::instance()->postEvent(
            new SIPNewRegistrationEvent(info,
                handle.empty() ? AmSession::getNewId() : handle,
                sess_link)
        );
    } else if(action=="remove") {
        if(!data.hasMember("id")) { ERROR("missed 'id' in BusReplyEvent payload");return; }
        AmArg &id_arg = data["id"];
        string id;
        if(isArgCStr(id_arg)) {
            id = id_arg.asCStr();
        } else if(isArgInt(id_arg)) {
            id = int2str(id_arg.asInt());
        } else {
            ERROR("unexpected 'id' type. expected string or integer");
            return;
        }
        removeRegistrationById(id);
    } else if(action=="flush") {
        DBG("flushRegistrations()");
        AmLock l(reg_mut);
        for(const auto &reg: registrations)
            reg.second->doUnregister();
        registrations.clear();
        registrations_by_id.clear();
    } else {
        ERROR("unknown action '%s'",action.c_str());
    }
#undef DEF_AND_VALIDATE_OPTIONAL_STR
}

void SIPRegistrarClient::onBusEvent(BusReplyEvent* bus_event)
{
    try {
        AmArg &data = bus_event->data;
        if(isArgArray(data)) {
            for (size_t i = 0; i < data.size(); i ++) {
                processAmArgRegistration(data[i]);
            }
        } else {
            processAmArgRegistration(data);
        }
    } catch(AmSession::Exception &e) {
        ERROR("onBusEvent() exception: %d %s",
              e.code,e.reason.c_str());
    } catch(...) {
        ERROR("onBusEvent(0) unknown exception");
    }
}

void SIPRegistrarClient::on_stop()
{
    stop_event.fire();
    stopped.wait_for();
}


bool SIPRegistrarClient::onSipReply(const AmSipReply& rep, AmSipDialog::Status old_dlg_status)
{
    DBG("got reply with tag '%s'", rep.from_tag.c_str());

    if (instance()->hasRegistration(rep.from_tag)) {
        instance()->postEvent(new AmSipReplyEvent(rep));
        return true;
    } else
        return false;
}

bool SIPRegistrarClient::hasRegistration(const string& handle)
{
    return get_reg(handle) != NULL;
}

AmSIPRegistration* SIPRegistrarClient::get_reg(const string& reg_id) 
{
    DBG("get registration '%s'", reg_id.c_str());
    AmSIPRegistration* res = NULL;
    reg_mut.lock();
    map<string, AmSIPRegistration*>::iterator it =
        registrations.find(reg_id);
    if (it!=registrations.end())
        res = it->second;
    reg_mut.unlock();
    DBG("get registration : res = '%ld' (this = %ld)", (long)res, (long)this);
    return res;
}

AmSIPRegistration* SIPRegistrarClient::get_reg_unsafe(const string& reg_id) 
{
    //	DBG("get registration_unsafe '%s'", reg_id.c_str());
    AmSIPRegistration* res = NULL;
    map<string, AmSIPRegistration*>::iterator it =
        registrations.find(reg_id);
    if (it!=registrations.end())
        res = it->second;
    //     DBG("get registration_unsafe : res = '%ld' (this = %ld)", (long)res, (long)this);
    return res;
}

AmSIPRegistration* SIPRegistrarClient::remove_reg(const string& reg_id)
{
    reg_mut.lock();
    AmSIPRegistration* reg = remove_reg_unsafe(reg_id);
    reg_mut.unlock();
    return reg;
}

AmSIPRegistration* SIPRegistrarClient::remove_reg_unsafe(const string& reg_id)
{
    DBG("removing registration %s", reg_id.c_str());
    AmSIPRegistration* reg = NULL;
    map<string, AmSIPRegistration*>::iterator it =
        registrations.find(reg_id);
    if (it!=registrations.end()) {
        reg = it->second;
        registrations.erase(it);
    }
    AmEventDispatcher::instance()->delEventQueue(reg_id);
    return reg;
}

bool SIPRegistrarClient::add_reg(const string& reg_id, AmSIPRegistration* new_reg)
{
    CLASS_DBG("adding registration '%s' with id = '%s'",
        reg_id.c_str(), new_reg->getInfo().id.c_str());
    AmSIPRegistration* reg = NULL;
    reg_mut.lock();
    map<string, AmSIPRegistration*>::iterator it =
        registrations.find(reg_id);
    if (it!=registrations.end()) {
        reg = it->second;
    }

    std::pair<RegHash::iterator,bool> ret =
        registrations_by_id.insert(
            RegHashPair(new_reg->getInfo().id,new_reg));
    if(!ret.second) {
        reg_mut.unlock();
        ERROR("duplicate id: %s on create registration %s",
            new_reg->getInfo().id.c_str(),
            reg_id.c_str());
        if (new_reg->getEventSink().length()) {
            AmSessionContainer::instance()->
                postEvent(new_reg->getEventSink(),
                    new SIPRegistrationEvent(
                        SIPRegistrationEvent::RegisterDuplicate,
                        new_reg->getHandle(), new_reg->getInfo().id));
        }
        delete new_reg;
        return false;
    }

    registrations[reg_id] = new_reg;

    AmEventDispatcher::instance()->addEventQueue(reg_id,this);
    reg_mut.unlock();

    if (reg != NULL)
        delete reg; // old one with the same ltag

    return true;
}


// API
string SIPRegistrarClient::createRegistration(
    const string& id,
    const string& domain,
    const string& user,
    const string& name,
    const string& auth_user,
    const string& pwd,
    const string& sess_link,
    const string& proxy,
    const string& contact,
    const int& expires_interval,
    bool &force_expires_interval,
    const int& retry_delay,
    const int& max_attempts,
    const int& transport_protocol_id,
    const int& proxy_transport_protocol_id,
    const int &transaction_timeout,
    const int &srv_failover_timeout,
    const string& handle,
    const dns_priority& priority,
    sip_uri::uri_scheme scheme_id = sip_uri::SIP)
{
    DBG("createRegistration");

    string l_handle = handle.empty() ? AmSession::getNewId() : handle;
    instance()->postEvent(
        new SIPNewRegistrationEvent(
            SIPRegistrationInfo(
                id,
                domain,
                user,
                name,
                auth_user,
                pwd,
                proxy,
                contact,
                string(),
                map<string, string>(),
                expires_interval,
                force_expires_interval,
                retry_delay,
                max_attempts,
                transport_protocol_id,
                proxy_transport_protocol_id,
                transaction_timeout,
                srv_failover_timeout,
                priority,
                scheme_id),
            l_handle,
            sess_link
        )
    );
    return l_handle;
}

void SIPRegistrarClient::removeRegistration(const string& handle)
{
    instance()->postEvent(new SIPRemoveRegistrationEvent(handle));
}

void SIPRegistrarClient::removeRegistrationById(const string& id)
{
    instance()->postEvent(new SIPRemoveRegistrationEvent(id,true));
}

bool SIPRegistrarClient::getRegistrationState(
    const string& handle,
    unsigned int& state,
    unsigned int& expires_left)
{
    bool res = false;
    reg_mut.lock();

    AmSIPRegistration* reg = get_reg_unsafe(handle);
    if (reg) {
        res = true;
        state = reg->getState();
        expires_left = reg->getExpiresLeft();
    }
    reg_mut.unlock();
    return res;
}

void SIPRegistrarClient::listRegistrations(AmArg& res)
{
    res.assertArray();
    reg_mut.lock();
    RegShaper::timep now(std::chrono::system_clock::now());
    for (map<string, AmSIPRegistration*>::iterator it =
         registrations.begin(); it != registrations.end(); it++)
    {
        reg2arg(it,res,now);
    }
    reg_mut.unlock();
}

void SIPRegistrarClient::showRegistration(const string& handle, AmArg &ret)
{
    AmLock l(reg_mut);
    map<string, AmSIPRegistration*>::iterator it = registrations.find(handle);
    ret.assertArray();
    if(it!=registrations.end())
        reg2arg(it,ret,std::chrono::system_clock::now());
}

void SIPRegistrarClient::showRegistrationById(const string& id, AmArg &ret)
{
    AmLock l(reg_mut);
    RegHash::iterator it = registrations_by_id.find(id);
    ret.assertArray();
    if(it!=registrations_by_id.end())
        reg2arg(it,ret,std::chrono::system_clock::now());
}

void SIPRegistrarClient::getRegistrationsCount(AmArg& res)
{
    reg_mut.lock();
    res = registrations.size();
    reg_mut.unlock();
}

void SIPRegistrarClient::invoke(
    const string& method,
    const AmArg& args,
    AmArg& ret)
{
    if(method == "createRegistration"){
        if(isArgStruct(args[0])) {

#define DEF_AND_VALIDATE_OPTIONAL_STR(key) \
    string key; \
    if(args[0].hasMember(#key)) { \
        AmArg & key ## _arg = args[0][#key]; \
        if(!isArgCStr(key ## _arg)) throw AmSession::Exception(500,"unexpected '" #key "' type. expected string");\
        key = key ## _arg.asCStr(); \
    }

            DEF_AND_VALIDATE_OPTIONAL_STR(handle);
            DEF_AND_VALIDATE_OPTIONAL_STR(sess_link);
            string l_handle = handle.empty() ? AmSession::getNewId() : handle;
            SIPRegistrationInfo info;
            info.init_from_amarg(args[0]);
            instance()->postEvent(new SIPNewRegistrationEvent(info,l_handle,sess_link));
            ret.push(true);

#undef DEF_AND_VALIDATE_OPTIONAL_STR

        } else {
            string proxy, contact, handle, sess_link;
            int expires_interval = 0,
                force = 0,
                retry_delay = DEFAULT_REGISTER_RETRY_DELAY,
                max_attempts = REGISTER_ATTEMPTS_UNLIMITED,
                transport_protocol_id = sip_transport::UDP,
                proxy_transport_protocol_id = sip_transport::UDP,
                scheme_id = sip_uri::SIP,
                transaction_timeout = 0,
                srv_failover_timeout = 0;
            bool force_expires_interval = false;
            dns_priority priority;
            size_t n = args.size();

            if(n < 6) {
                throw AmSession::Exception(500,"expected at least 6 args");
            }

            for(int i = 0; i < 6; i++) {
                if(!isArgCStr(args.get(i))) {
                    throw AmSession::Exception(500,"expected string at arg: " + int2str(i+1));
                }
            }

#define DEF_AND_VALIDATE_OPTIONAL_STR(index, name) \
            if (args.size() > index) {\
                AmArg &a = args.get(index);\
                if(!isArgUndef(a)) {\
                    if(!isArgCStr(a))\
                        throw AmSession::Exception(500,"wrong " #name " arg. expected string or null");\
                    name = a.asCStr();\
                }\
            } else break

#define DEF_AND_VALIDATE_OPTIONAL_INT(index, name) \
            if (args.size() > index) {\
                AmArg &a = args.get(index);\
                if(isArgInt(a)) {\
                    name = a.asInt();\
                } else if(isArgCStr(a) && !str2int(a.asCStr(), name)){\
                    throw AmSession::Exception(500,"wrong " #name " argument");\
                }\
            } else break

            do {
                string priority_str;
                DEF_AND_VALIDATE_OPTIONAL_STR(6, sess_link);
                DEF_AND_VALIDATE_OPTIONAL_STR(7, proxy);
                DEF_AND_VALIDATE_OPTIONAL_STR(8, contact);
                DEF_AND_VALIDATE_OPTIONAL_INT(9, expires_interval);
                DEF_AND_VALIDATE_OPTIONAL_INT(10, force);
                DEF_AND_VALIDATE_OPTIONAL_INT(11, retry_delay);
                DEF_AND_VALIDATE_OPTIONAL_INT(12, max_attempts);
                DEF_AND_VALIDATE_OPTIONAL_INT(13, transport_protocol_id);
                DEF_AND_VALIDATE_OPTIONAL_INT(14, proxy_transport_protocol_id);
                DEF_AND_VALIDATE_OPTIONAL_INT(15, transaction_timeout);
                DEF_AND_VALIDATE_OPTIONAL_INT(16, srv_failover_timeout);
                DEF_AND_VALIDATE_OPTIONAL_STR(17, handle);
                DEF_AND_VALIDATE_OPTIONAL_INT(18, scheme_id);
                DEF_AND_VALIDATE_OPTIONAL_STR(19, priority_str);

                priority = string_to_priority(priority_str);

                if(scheme_id < sip_uri::SIP || scheme_id > sip_uri::SIPS) {
                    throw AmSession::Exception(500,"unexpected scheme_id value");
                }
            } while(0);
#undef DEF_AND_VALIDATE_OPTIONAL_STR
#undef DEF_AND_VALIDATE_OPTIONAL_INT

            ret.push(createRegistration(
                args.get(0).asCStr(),
                args.get(1).asCStr(),
                args.get(2).asCStr(),
                args.get(3).asCStr(),
                args.get(4).asCStr(),
                args.get(5).asCStr(),
                sess_link,
                proxy,
                contact,
                expires_interval,
                force_expires_interval,
                retry_delay,
                max_attempts,
                transport_protocol_id,
                proxy_transport_protocol_id,
                transaction_timeout,
                srv_failover_timeout,
                handle,
                priority,
                static_cast<sip_uri::uri_scheme>(scheme_id)));
        }
    } else if(method == "removeRegistration") {
        removeRegistration(args.get(0).asCStr());
    } else if(method == "removeRegistrationById") {
        removeRegistrationById(args.get(0).asCStr());
    } else if(method == "getRegistrationState") {
        unsigned int state;
        unsigned int expires;
        if (instance()->getRegistrationState(args.get(0).asCStr(),
            state, expires))
        {
            ret.push(1);
            ret.push((int)state);
            ret.push((int)expires);
        } else {
            ret.push(AmArg((int)0));
        }
    } else if(method == "listRegistrations") {
        listRegistrations(ret);
    } else if(method == "showRegistration") {
        showRegistration(args.get(0).asCStr(),ret);
    } else if(method == "showRegistrationById") {
        showRegistrationById(args.get(0).asCStr(),ret);
    } else if(method == "getRegistrationsCount") {
        getRegistrationsCount(ret);
    } else if(method == "_list") {
        ret.push(AmArg("createRegistration"));
        ret.push(AmArg("removeRegistration"));
        ret.push(AmArg("removeRegistrationById"));
        ret.push(AmArg("getRegistrationState"));
        ret.push(AmArg("listRegistrations"));
        ret.push(AmArg("showRegistration"));
        ret.push(AmArg("showRegistrationById"));
        ret.push(AmArg("getRegistrationsCount"));
    }  else
        throw AmDynInvoke::NotImplemented(method);
}

struct RegistrationMetricGroup
  : public StatCountersGroupsInterface
{
    static vector<string> metrics_keys_names;
    static vector<string> metrics_help_strings;

    enum metric_keys_idx {
        REG_VALUE_POSTPONE_TIMEOUT_MSEC = 0,
        REG_VALUE_ATTEMPT,
        REG_VALUE_STATE,
        REG_VALUE_MAX
    };
    struct reg_info {
        map<string, string> labels;
        unsigned long long values[REG_VALUE_MAX];
    };
    vector<reg_info> data;
    int idx;

    RegistrationMetricGroup()
      : StatCountersGroupsInterface(Gauge)
    {}

    void add_reg(RegShaper::timep &now, const string &handle, AmSIPRegistration &reg)
    {
        data.emplace_back();
        auto &labels = data.back().labels;
        auto &values = data.back().values;
        auto &ri = reg.getInfo();

        labels["handle"] = handle;
        labels["id"] = ri.id;

        labels["domain"] = ri.domain;
        labels["transport_protocol"] = c2stlstr(transport_str(ri.transport_protocol_id));

        labels["user"] = ri.user;
        labels["auth_user"] = ri.auth_user;
        labels["expires_interval"] = int2str(ri.expires_interval);
        labels["transport_protocol"] = c2stlstr(transport_str(ri.transport_protocol_id));
        labels["contact"] = reg.request_contact;

        if(!ri.proxy.empty()) {
            labels["proxy"] = ri.proxy;
            labels["proxy_transport_protocol"] = c2stlstr(transport_str(ri.proxy_transport_protocol_id));
        }

        auto reg_state = reg.getState();
        if(reg_state==AmSIPRegistration::RegisterError) {
            labels["error_code"] = int2str(reg.error_code);
            labels["error_reason"] = reg.error_reason;
            labels["error_initiator"] = getSIPRegistationErrorInitiatorString(reg.error_initiatior);
        }

        values[REG_VALUE_POSTPONE_TIMEOUT_MSEC] = reg.postponed ?
            std::chrono::duration_cast<std::chrono::milliseconds>(
                reg.postponed_next_attempt-now).count() : 0;
        values[REG_VALUE_ATTEMPT] = ri.attempt;
        values[REG_VALUE_STATE] = reg_state;
    }

    void serialize(StatsCountersGroupsContainerInterface::iterate_groups_callback_type callback)
    {
        for(int i = 0; i < REG_VALUE_MAX; i++) {
            idx = i;
            setHelp(metrics_help_strings[idx]);
            callback(metrics_keys_names[idx], *this);
        }
    }

    void iterate_counters(iterate_counters_callback_type callback) override
    {
        for (size_t i = 0; i < data.size(); i++) {
            auto &reg = data[i];
            callback(reg.values[idx], /*0,*/ reg.labels);
        }
    }
};

vector<string> RegistrationMetricGroup::metrics_keys_names = {
    "registration_postpone_timeout_msec",
    "registration_attempt",
    "registration_state"
};

vector<string> RegistrationMetricGroup::metrics_help_strings = {
    "",
    "",
    "0:pending, 1:active, 2:error, 3:expired, 4:postponed"
};

void SIPRegistrarClient::operator ()(const string &name, iterate_groups_callback_type callback)
{
    AmArg ret;
    ret.assertArray();

    RegistrationMetricGroup g;
    {
        AmLock l(reg_mut);
        RegShaper::timep now(std::chrono::system_clock::now());
        g.data.reserve(registrations.size());
        for(const auto &reg_it: registrations) {
            g.add_reg(now, reg_it.first, *reg_it.second);
        }
    }

    g.serialize(callback);
}
