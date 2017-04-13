
#include <map>
#include <set>
#include <list>
#include <iostream>
#include <typeinfo>

#include <stdlib.h>

#include <epicsMutex.h>
#include <epicsGuard.h>

#include <pv/pvAccess.h>
#include <pv/logger.h>
#include <pv/clientFactory.h>
#include <pv/caProvider.h>
#include <pv/configuration.h>

#include "p4p.h"

namespace {

typedef epicsGuard<epicsMutex> Guard;

namespace pvd = epics::pvData;
namespace pva = epics::pvAccess;

struct Context;
struct Channel;
struct OpBase;

struct Context {
    POINTER_DEFINITIONS(Context);

    pva::ChannelProvider::shared_pointer provider;

    Context() {}
    ~Context() { close(); }

    void close();

    static int       py_init(PyObject *self, PyObject *args, PyObject *kws);
    static PyObject *py_channel(PyObject *self, PyObject *args, PyObject *kws);
    static PyObject *py_close(PyObject *self);

    static PyObject *py_providers(PyObject *junk);
    static PyObject *py_set_debug(PyObject *junk, PyObject *args, PyObject *kws);
    static PyObject *py_makeRequest(PyObject *junk, PyObject *args);
};

struct Channel {
    POINTER_DEFINITIONS(Channel);

    struct Req : public pva::ChannelRequester {
        POINTER_DEFINITIONS(Req);

        Channel::weak_pointer owner;
        Req(const Channel::shared_pointer& o) : owner(o) {}
        virtual ~Req() {}

        virtual std::string getRequesterName() { return "p4p.Channel"; }

        virtual void channelCreated(const pvd::Status& status, pva::Channel::shared_pointer const & channel);
        virtual void channelStateChange(pva::Channel::shared_pointer const & channel, pva::Channel::ConnectionState connectionState);
    };

    pva::Channel::shared_pointer channel;

    typedef std::set<std::tr1::shared_ptr<OpBase> > operations_t;
    operations_t ops;

    static int py_init(PyObject *self, PyObject *args, PyObject *kws);
    static PyObject *py_get(PyObject *self, PyObject *args, PyObject *kws);
    static PyObject *py_put(PyObject *self, PyObject *args, PyObject *kws);
    static PyObject *py_rpc(PyObject *self, PyObject *args, PyObject *kws);
    static PyObject *py_name(PyObject *self);
    static PyObject *py_close(PyObject *self);
};

struct OpBase {
    POINTER_DEFINITIONS(OpBase);

    Channel::shared_pointer channel;
    pvd::PVStructure::shared_pointer req;
    // completion callback
    PyRef cb;
    // put value
    PyRef pyvalue;
    // rpc value
    pvd::PVStructure::shared_pointer pvvalue;

    OpBase(const Channel::shared_pointer& ch) :channel(ch) {}
    virtual ~OpBase() {
        PyLock L;
        cancel();
    }

    void call_cb(PyObject *obj) {
        PyRef temp;
        cb.swap(temp);
        if(!temp.get()) return;
        PyObject *junk = PyObject_CallFunctionObjArgs(temp.get(), obj, NULL);
        if(junk) {
            Py_DECREF(junk);
        } else {
            PyErr_Print();
            PyErr_Clear();
        }
    }

    // pva::Channel life-cycle callbacks
    //  called to (re)start operation
    virtual void restart(const OpBase::shared_pointer& self) =0;
    //  channel lost connection
    virtual void lostConn(const OpBase::shared_pointer& self) =0;
    //  channel destoryed or user cancel
    virtual bool cancel() {
        TRACE("chan="<<channel.get()<<" "<<typeid(this).name());
        if(!channel) return false;
        bool found = false;
        for(Channel::operations_t::iterator it = channel->ops.begin(), end = channel->ops.end(); it!=end; ++it)
        {
            if(it->get()==this) {
                found = true;
                TRACE("remove "<<it->use_count());
                channel->ops.erase(it);
                break;
            }
        }
        channel.reset();
        cb.reset();
        return found;
    }

    // called with GIL locked
    void destroy() { cancel(); }

    static PyObject *py_cancel(PyObject *self);
    static int py_traverse(PyObject *self, visitproc visit, void *arg);
    static int py_clear(PyObject *self);

    virtual int traverse(visitproc visit, void *arg)
    {
        if(cb.get())
            Py_VISIT(cb.get());
        if(pyvalue.get())
            Py_VISIT(pyvalue.get());
        return 0;
    }

    virtual void clear()
    {
        // ~= Py_CLEAR(cb)
        {
            PyRef tmp;
            cb.swap(tmp);
        }
        {
            PyRef tmp;
            pyvalue.swap(tmp);
        }
    }
};

typedef PyClassWrapper<Context> PyContext;
typedef PyClassWrapper<std::tr1::shared_ptr<Channel> > PyChannel;
typedef PyClassWrapper<std::tr1::shared_ptr<OpBase> > PyOp;

struct GetOp : public OpBase {
    POINTER_DEFINITIONS(GetOp);

    struct Req : public pva::ChannelGetRequester {
        POINTER_DEFINITIONS(Req);

        GetOp::weak_pointer owner;
        Req(const GetOp::shared_pointer& o) : owner(o) {}
        virtual ~Req() {}

        virtual std::string getRequesterName() { return "p4p.GetOp"; }

        virtual void channelGetConnect(
            const pvd::Status& status,
            pva::ChannelGet::shared_pointer const & channelGet,
            pvd::Structure::const_shared_pointer const & structure);

        virtual void getDone(
            const pvd::Status& status,
            pva::ChannelGet::shared_pointer const & channelGet,
            pvd::PVStructure::shared_pointer const & pvStructure,
            pvd::BitSet::shared_pointer const & bitSet);
    };

    pva::ChannelGet::shared_pointer op;

    GetOp(const Channel::shared_pointer& ch) :OpBase(ch) {}
    virtual ~GetOp() {}

    virtual void restart(const OpBase::shared_pointer &self);
    virtual void lostConn(const OpBase::shared_pointer& self);
    virtual bool cancel();
};

struct PutOp : public OpBase {
    POINTER_DEFINITIONS(PutOp);

    struct Req : public pva::ChannelPutRequester {
        POINTER_DEFINITIONS(Req);

        PutOp::weak_pointer owner;
        Req(const PutOp::shared_pointer& o) : owner(o) {}
        virtual ~Req() {}

        virtual std::string getRequesterName() { return "p4p.PutOp"; }

        virtual void channelPutConnect(
            const pvd::Status& status,
            pva::ChannelPut::shared_pointer const & channelPut,
            pvd::Structure::const_shared_pointer const & structure);

        virtual void putDone(
            const pvd::Status& status,
            pva::ChannelPut::shared_pointer const & channelPut);

        virtual void getDone(
            const pvd::Status& status,
            pva::ChannelPut::shared_pointer const & channelPut,
            pvd::PVStructure::shared_pointer const & pvStructure,
            pvd::BitSet::shared_pointer const & bitSet)
        { /* no used */ }
    };

    pva::ChannelPut::shared_pointer op;
    // sent once the network op may have gone out.
    // the point at which we can't retry safely
    bool sent;

    PutOp(const Channel::shared_pointer& ch) :OpBase(ch), sent(false) {}
    virtual ~PutOp() {}

    virtual void restart(const OpBase::shared_pointer &self);
    virtual void lostConn(const OpBase::shared_pointer& self);
    virtual bool cancel();
};

struct RPCOp : public OpBase {
    POINTER_DEFINITIONS(RPCOp);

    struct Req : public pva::ChannelRPCRequester {
        POINTER_DEFINITIONS(Req);

        RPCOp::weak_pointer owner;
        Req(const RPCOp::shared_pointer& o) : owner(o) {}
        virtual ~Req() {}

        virtual std::string getRequesterName() { return "p4p.PutOp"; }

        virtual void channelRPCConnect(
            const pvd::Status& status,
            pva::ChannelRPC::shared_pointer const & channelRPC);

        virtual void requestDone(
            const pvd::Status& status,
            pva::ChannelRPC::shared_pointer const & channelRPC,
            pvd::PVStructure::shared_pointer const & pvResponse);
    };

    pva::ChannelRPC::shared_pointer op;
    // sent once the network op may have gone out.
    // the point at which we can't retry safely
    bool sent;

    RPCOp(const Channel::shared_pointer& ch) :OpBase(ch), sent(false) {}
    virtual ~RPCOp() {}

    virtual void restart(const OpBase::shared_pointer &self);
    virtual void lostConn(const OpBase::shared_pointer& self);
    virtual bool cancel();
};

#define TRY PyContext::reference_type SELF = PyContext::unwrap(self); try


int Context::py_init(PyObject *self, PyObject *args, PyObject *kws)
{
    TRY {
        static const char* names[] = {"provider", "conf", "useenv", NULL};
        const char *pname;
        PyObject *cdict = Py_None, *useenv = Py_True;
        if(!PyArg_ParseTupleAndKeywords(args, kws, "s|OO", (char**)names, &pname, &cdict, &useenv))
            return -1;

        pva::ConfigurationBuilder B;

        if(PyObject_IsTrue(useenv))
            B.push_env();

        if(cdict==Py_None) {
            // nothing
        } else if(PyDict_Check(cdict)) {
            Py_ssize_t I = 0;
            PyObject *key, *value;

            while(PyDict_Next(cdict, &I, &key, &value)) {
                PyString K(key), V(value);

                B.add(K.str(), V.str());
            }

            B.push_map();
        } else {
            PyErr_Format(PyExc_ValueError, "conf=%s not valid", Py_TYPE(cdict)->tp_name);
            return -1;
        }

        // we create our own provider.
        // we are greedy and don't want to share (also we can destroy channels at will)
#if 0
        // No way to apply custom config :P
        SELF.provider = pva::getChannelProviderRegistry()->createProvider(pname);
#else
        SELF.provider = pva::getChannelProviderRegistry()->createProvider(pname, B.build());
#endif

        TRACE("Context init");

        if(!SELF.provider)
            throw std::logic_error("createProvider returns NULL");

        return 0;
    } CATCH()
    return -1;
}

PyObject *Context::py_channel(PyObject *self, PyObject *args, PyObject *kws)
{
    TRY {
        static const char* names[] = {"channel", NULL};
        char *cname;
        if(!PyArg_ParseTupleAndKeywords(args, kws, "s", (char**)names, &cname))
            return NULL;

        if(!SELF.provider)
            return PyErr_Format(PyExc_RuntimeError, "Context has been closed");

        PyRef klass(PyObject_GetAttrString(self, "Channel"));
        if(!PyType_Check(klass.get()) || !PyType_IsSubtype((PyTypeObject*)klass.get(), &PyChannel::type))
            return PyErr_Format(PyExc_RuntimeError, "self.Channel not valid");
        PyTypeObject *chanklass = (PyTypeObject*)klass.get();

        Channel::shared_pointer pychan(new Channel);
        Channel::Req::shared_pointer pyreq(new Channel::Req(pychan));

        pva::Channel::shared_pointer chan;

        std::string chanName(cname);

        {
            PyUnlock U;
            chan = SELF.provider->createChannel(chanName, pyreq);
        }

        if(!chan)
            return PyErr_Format(PyExc_RuntimeError, "Failed to create channel '%s'", cname);

        pychan->channel = chan;

        PyRef ret(chanklass->tp_new(chanklass, args, kws));

        PyChannel::unwrap(ret.get()).swap(pychan);

        if(chanklass->tp_init && chanklass->tp_init(ret.get(), args, kws))
            throw std::runtime_error("Error Channel.__init__");

        TRACE("Channel "<<cname<<" "<<chan);
        return ret.release();
    } CATCH()
    return NULL;
}

void Context::close()
{
    TRACE("Context close");
    if(provider) {
        PyUnlock U;
        provider.reset();
    }
}

PyObject *Context::py_close(PyObject *self)
{
    TRY {
        SELF.close();
        Py_RETURN_NONE;
    } CATCH()
    return NULL;
}

PyObject*  Context::py_providers(PyObject *junk)
{
    try {
        std::auto_ptr<pva::ChannelProviderRegistry::stringVector_t> names(pva::getChannelProviderRegistry()->getProviderNames());

        if(!names.get())
            return PyErr_Format(PyExc_RuntimeError, "Unable for fetch provider names!?!");

        PyRef ret(PyList_New(names->size()));

        for(size_t i=0; i<names->size(); i++) {
            PyRef name(PyUnicode_FromString((*names)[i].c_str()));

            PyList_SET_ITEM(ret.get(), i, name.release());
        }

        return ret.release();
    }CATCH()
    return NULL;
}

PyObject*  Context::py_set_debug(PyObject *junk, PyObject *args, PyObject *kws)
{
    try {
        int lvl = pva::logLevelError;
        static const char* names[] = {"level", NULL};
        if(!PyArg_ParseTupleAndKeywords(args, kws, "|i", (char**)&names, &lvl))
            return NULL;

        pva::pvAccessSetLogLevel((pva::pvAccessLogLevel)lvl);

        Py_RETURN_NONE;
    }CATCH()
    return NULL;
}

PyObject* Context::py_makeRequest(PyObject *junk, PyObject *args)
{
    try {
        const char *req;
        if(!PyArg_ParseTuple(args, "s", &req))
            return NULL;

        // OMG, would a simple function w/ 'throw' be so much to ask for!?!?!
        pvd::CreateRequest::shared_pointer create(pvd::CreateRequest::create());
        pvd::PVStructure::shared_pointer str(create->createRequest(req));
        if(!str)
            throw std::runtime_error(SB()<<"Error parsing pvRequest: "<<create->getMessage());

        PyRef ret(P4PValue_wrap(P4PValue_type, str));

        return ret.release();
    }CATCH()
    return NULL;
}

pvd::PVStructure::shared_pointer buildRequest(PyObject *req)
{
    pvd::PVStructure::shared_pointer opts;

    /*TODO:
     *  None -> empty struct
     *  str  -> parse from pvRequest min-language
     *  {}   -> Translate directly
     *
     */
    if(req==Py_None) {
        // create {'field':{}} to get everything
        opts = pvd::getPVDataCreate()->createPVStructure(pvd::getFieldCreate()->createFieldBuilder()
                                                         ->addNestedStructure("field")
                                                         ->endNested()
                                                         ->createStructure());

    } else if(PyBytes_Check(req) || PyUnicode_Check(req)) {
        PyString S(req);
        std::string R(S.str());
        if(R.empty())
            R = "field()"; // ensure some sane default (get everything)

        pvd::CreateRequest::shared_pointer create(pvd::CreateRequest::create());
        opts = create->createRequest(R);
        if(!opts)
            throw std::runtime_error(SB()<<"Error parsing pvRequest \""<<R<<"\" : "<<create->getMessage());

    } else {
        opts = P4PValue_unwrap(req);
    }
    return opts;
}

#undef TRY
#define TRY PyChannel::reference_type SELF = PyChannel::unwrap(self); try

int Channel::py_init(PyObject *self, PyObject *args, PyObject *kws)
{
    TRY {
        // ignores arguments
        if(!SELF->channel) {
            PyErr_Format(PyExc_ValueError, "Can't construct Channel explicitly");
            return 1;
        }

        return 0;
    }CATCH();
    return 1;
}

PyObject* Channel::py_get(PyObject *self, PyObject *args, PyObject *kws)
{
    TRY {
        static const char *names[] = {"callback", "request", NULL};
        PyObject *cb, *req = Py_None;
        if(!PyArg_ParseTupleAndKeywords(args, kws, "O|O", (char**)names, &cb, &req))
            return NULL;

        if(!PyCallable_Check(cb))
            return PyErr_Format(PyExc_ValueError, "callable required, not %s", Py_TYPE(cb)->tp_name);

        if(!SELF->channel)
            return PyErr_Format(PyExc_RuntimeError, "Channel closed");

        TRACE("Channel get "<<SELF->channel->getChannelName());

        GetOp::shared_pointer reqop(new GetOp(SELF));
        reqop->cb.reset(cb, borrow());
        reqop->req = buildRequest(req);

        //TODO: PVA provider lets us start get() when not connected
        //      CA provider fails.
        //      Race with connection test?

        if(SELF->channel->isConnected()) {
            TRACE("Issue get");
            reqop->restart(reqop);
        } else {
            TRACE("Wait for connect refs="<<reqop.use_count());
            SELF->ops.insert(reqop);
        }

        try {
            PyRef ret(PyOp::type.tp_new(&PyOp::type, args, kws));

            PyOp::unwrap(ret.get()) = reqop;

            return ret.release();
        }catch(...) {
            reqop->op->destroy();
            SELF->ops.erase(reqop);
            throw;
        }
    }CATCH()
    return NULL;
}

PyObject* Channel::py_put(PyObject *self, PyObject *args, PyObject *kws)
{
    TRY {
        static const char *names[] = {"callback", "value", "request", NULL};
        PyObject *cb, *val, *req = Py_None;
        if(!PyArg_ParseTupleAndKeywords(args, kws, "OO|O", (char**)names, &cb, &val, &req))
            return NULL;

        if(!PyCallable_Check(cb))
            return PyErr_Format(PyExc_ValueError, "callable required, not %s", Py_TYPE(cb)->tp_name);

        if(PyObject_IsInstance(val, (PyObject*)P4PValue_type)) { /* ok */ }
        else if(PyCallable_Check(val)) { /* ok */ }
        else {
            return PyErr_Format(PyExc_ValueError, "put value must be Value or callable which returns Value");
        }

        if(!SELF->channel)
            return PyErr_Format(PyExc_RuntimeError, "Channel closed");

        TRACE("Channel put "<<SELF->channel->getChannelName());

        PutOp::shared_pointer reqop(new PutOp(SELF));
        reqop->cb.reset(cb, borrow());
        reqop->pyvalue.reset(val, borrow());
        reqop->req = buildRequest(req);

        if(SELF->channel->isConnected()) {
            TRACE("Issue put");
            reqop->restart(reqop);
        } else {
            TRACE("Wait for connect");
            SELF->ops.insert(reqop);
        }

        try {
            PyRef ret(PyOp::type.tp_new(&PyOp::type, args, kws));

            PyOp::unwrap(ret.get()) = reqop;

            return ret.release();
        }catch(...) {
            reqop->op->destroy();
            SELF->ops.erase(reqop);
            throw;
        }
    }CATCH()
    return NULL;
}

PyObject* Channel::py_rpc(PyObject *self, PyObject *args, PyObject *kws)
{
    TRY {
        static const char *names[] = {"callback", "value", "request", NULL};
        PyObject *cb, *val, *req = Py_None;
        if(!PyArg_ParseTupleAndKeywords(args, kws, "OO!|O", (char**)names, &cb, P4PValue_type, &val, &req))
            return NULL;

        if(!PyCallable_Check(cb))
            return PyErr_Format(PyExc_ValueError, "callable required, not %s", Py_TYPE(cb)->tp_name);

        if(!SELF->channel)
            return PyErr_Format(PyExc_RuntimeError, "Channel closed");

        TRACE("Channel rpc "<<SELF->channel->getChannelName());

        RPCOp::shared_pointer reqop(new RPCOp(SELF));
        reqop->cb.reset(cb, borrow());
        reqop->pvvalue = P4PValue_unwrap(val);
        reqop->req = buildRequest(req);

        if(SELF->channel->isConnected()) {
            TRACE("Issue rpc");
            reqop->restart(reqop);
        } else {
            TRACE("Wait for connect");
            SELF->ops.insert(reqop);
        }

        try {
            PyRef ret(PyOp::type.tp_new(&PyOp::type, args, kws));

            PyOp::unwrap(ret.get()) = reqop;

            return ret.release();
        }catch(...) {
            reqop->op->destroy();
            SELF->ops.erase(reqop);
            throw;
        }
    }CATCH()
    return NULL;
}

PyObject* Channel::py_name(PyObject *self)
{
    TRY {
        if(!SELF->channel)
            return PyErr_Format(PyExc_RuntimeError, "Channel closed");

        return PyUnicode_FromString(SELF->channel->getChannelName().c_str());
    }CATCH();
    return NULL;
}

PyObject *Channel::py_close(PyObject *self)
{
    TRY {
        if(SELF->channel) {
            pva::Channel::shared_pointer chan;
            SELF->channel.swap(chan);
            {
                PyUnlock U;
                chan->destroy();
                chan.reset();
            }
        }
        Py_RETURN_NONE;
    }CATCH();
    return NULL;
}

void Channel::Req::channelCreated(const pvd::Status& status, pva::Channel::shared_pointer const & channel)
{
    //TODO: can/do client contexts signal any errors here?
    TRACE(channel->getChannelName()<<" "<<status);
    if(!status.isOK()) {
        std::cout<<"Warning: unexpected in "<<__FUNCTION__<<" "<<status<<"\n";
    }
    (void)channel;
}

void Channel::Req::channelStateChange(pva::Channel::shared_pointer const & channel, pva::Channel::ConnectionState connectionState)
{
    Channel::shared_pointer op(owner.lock());
    if(!op)
        return;
    PyLock L;
    TRACE(channel->getChannelName()<<" "<<connectionState<<" #ops="<<op->ops.size());
    switch(connectionState) {
    case pva::Channel::NEVER_CONNECTED:
        break; // should never happen
    case pva::Channel::CONNECTED:
    {
        operations_t temp;
        temp.swap(op->ops);

        for(operations_t::const_iterator it = temp.begin(), end = temp.end(); it!=end; ++it) {
            TRACE("CONN "<<(*it));
            if(!(*it)) continue; // shouldn't happen, but guard against it anyway
            TRACE("CONN2 "<<(*it)<<" refs="<<it->use_count());
            try {
                (*it)->restart(*it);
                // restart() should re-add itself to ops
            } catch(std::exception& e) {
                std::cout<<"Error in restart() "<<e.what()<<"\n";
            }
        }
    }
        return;
    case pva::Channel::DISCONNECTED:
    {
        operations_t temp;
        temp.swap(op->ops);
        for(operations_t::const_iterator it = temp.begin(), end = temp.end(); it!=end; ++it) {
            if(!(*it)) continue; // shouldn't happen, but guard against it anyway
            try {
                (*it)->lostConn(*it);
            } catch(std::exception& e) {
                std::cout<<"Error in cancel() "<<e.what()<<"\n";
            }
        }
    }
        return;
    case pva::Channel::DESTROYED:
    {
        operations_t temp;
        temp.swap(op->ops);
        for(operations_t::const_iterator it = temp.begin(), end = temp.end(); it!=end; ++it) {
            if(!(*it)) continue; // shouldn't happen, but guard against it anyway
            try {
                (*it)->cancel();
            } catch(std::exception& e) {
                std::cout<<"Error in cancel() "<<e.what()<<"\n";
            }
        }
    }
        return;
    }
    std::cerr<<"channelStateChange(\""<<channel->getChannelName()<<"\", "<<connectionState<<") unexpected state\n";
}


#undef TRY
#define TRY PyOp::reference_type SELF = PyOp::unwrap(self); try

PyObject* OpBase::py_cancel(PyObject *self)
{
    TRY {
        bool cancelled = SELF->channel.get() && SELF->cancel();

        return PyBool_FromLong(cancelled);
    } CATCH()
    return NULL;
}

int OpBase::py_traverse(PyObject *self, visitproc visit, void *arg)
{
    TRY {
        return SELF->traverse(visit, arg);
    } CATCH()
    return -1;
}

int OpBase::py_clear(PyObject *self)
{
    TRY {
        SELF->clear();
        return 0;
    } CATCH()
    return -1;
}





void GetOp::restart(const OpBase::shared_pointer& self)
{
    TRACE("channel="<<channel.get()<<" refs="<<self.use_count());
    if(!channel) return;
    pva::ChannelGet::shared_pointer temp;
    GetOp::Req::shared_pointer pyreq(new GetOp::Req(std::tr1::static_pointer_cast<GetOp>(self)));
    temp.swap(op);
    {
        PyUnlock U;
        if(temp)
            temp->destroy();

        temp = channel->channel->createChannelGet(pyreq, req);
        TRACE("start get "<<temp<<" refs="<<self.use_count()<<" req="<<req);
    }
    op = temp;
    channel->ops.insert(self);
}

void GetOp::lostConn(const OpBase::shared_pointer &self)
{
    if(!channel) return;
    channel->ops.insert(self);
    if(op) {
        pva::ChannelGet::shared_pointer temp;
        temp.swap(op);

        PyUnlock U;

        temp->destroy();
        temp.reset();
    }
}

bool GetOp::cancel()
{
    bool canceled = OpBase::cancel();

    if(op) {
        canceled = true;
        pva::ChannelGet::shared_pointer temp;
        temp.swap(op);

        PyUnlock U;

        TRACE("destroy ChannelGet");
        temp->destroy();

        if(!temp.unique()) {
            TRACE("remaining refs="<<temp.use_count());
        }

        temp.reset();
    }

    return canceled;
}

void GetOp::Req::channelGetConnect(
    const pvd::Status& status,
    pva::ChannelGet::shared_pointer const & channelGet,
    pvd::Structure::const_shared_pointer const & structure)
{
    GetOp::shared_pointer op(owner.lock());
    if(!op)
        return;

    TRACE("get start "<<channelGet->getChannel()->getChannelName()<<" "<<status);
    if(!status.isSuccess()) {
        PyLock L;
        PyRef E(PyObject_CallFunction(PyExc_RuntimeError, (char*)"s", status.getMessage().c_str()));
        op->call_cb(E.get());
    } else {
        channelGet->lastRequest();
        // may call getDone() recursively
        channelGet->get();
    }
}

void GetOp::Req::getDone(
    const pvd::Status& status,
    pva::ChannelGet::shared_pointer const & channelGet,
    pvd::PVStructure::shared_pointer const & pvStructure,
    pvd::BitSet::shared_pointer const & bitSet)
{
    GetOp::shared_pointer op(owner.lock());
    if(!op)
        return;
    PyLock L;

    TRACE("get complete "<<channelGet->getChannel()->getChannelName()<<" for "<<op->cb.get()<<" with "<<status);
    if(!op->cb.get()) return;
    PyRef V;

    if(status.isSuccess()) {
        // we don't re-use ChannelGet, so assume exclusive ownership of pvStructure w/o a copy
        V.reset(P4PValue_wrap(P4PValue_type, pvStructure));
    } else {
        // build Exception instance
        // TODO: create RemoteError type
        V.reset(PyObject_CallFunction(PyExc_RuntimeError, (char*)"s", status.getMessage().c_str()));
    }

    if(!V.get()) {
        PyErr_Print();
        PyErr_Clear();
    } else {
        op->call_cb(V.get());
    }
}

void PutOp::restart(const OpBase::shared_pointer& self)
{
    TRACE("channel="<<channel.get()<<" sent="<<sent);
    if(!channel || sent) return;
    pva::ChannelPut::shared_pointer temp;
    PutOp::Req::shared_pointer pyreq(new PutOp::Req(std::tr1::static_pointer_cast<PutOp>(self)));
    temp.swap(op);
    {
        PyUnlock U;
        if(temp)
            temp->destroy();

        temp = channel->channel->createChannelPut(pyreq, req);
        TRACE("start put "<<temp);
    }
    op = temp;
    channel->ops.insert(self);
}

void PutOp::lostConn(const OpBase::shared_pointer &self)
{
    if(!channel) return;
    channel->ops.insert(self);
    if(op) {
        pva::ChannelPut::shared_pointer temp;
        temp.swap(op);

        PyUnlock U;

        temp->destroy();
        temp.reset();
    }
    if(sent) {
        PyRef err(PyObject_CallFunction(PyExc_RuntimeError, "s", "Connection lost before put acknowledged"));
        call_cb(err.get());
    }
}

bool PutOp::cancel()
{
    bool canceled = OpBase::cancel();
    sent=true;

    if(op) {
        canceled = true;
        pva::ChannelPut::shared_pointer temp;
        temp.swap(op);

        PyUnlock U;

        temp->destroy();
        temp.reset();
    }

    return canceled;
}

void PutOp::Req::channelPutConnect(
    const pvd::Status& status,
    pva::ChannelPut::shared_pointer const & channelPut,
    pvd::Structure::const_shared_pointer const & structure)
{
    PutOp::shared_pointer op(owner.lock());
    if(!op)
        return;

    TRACE("put start "<<channelPut->getChannel()->getChannelName()<<" "<<status);

    if(!status.isSuccess()) {
        PyLock L;
        PyRef err(PyObject_CallFunction(PyExc_RuntimeError, "s", status.getMessage().c_str()));
        op->call_cb(err.get());
    } else {
        pvd::PVStructure::shared_pointer val;
        {
            PyLock L;
            try {
                PyRef temp;
                temp.swap(op->pyvalue);
                if(!temp.get()) {
                    TRACE("no value!?!?!");
                    return;
                }
                //TODO: bitset?
                if(!PyObject_IsInstance(temp.get(), (PyObject*)P4PValue_type)) {
                    // assume callable
                    PyRef ptype(P4PType_wrap(P4PType_type, structure));
                    PyRef val(PyObject_CallFunctionObjArgs(temp.get(), ptype.get(), NULL));
                    temp.swap(val);
                }
                if(!PyObject_IsInstance(temp.get(), (PyObject*)P4PValue_type)) {
                    std::ostringstream msg;
                    msg<<"Can't put type \""<<Py_TYPE(temp.get())->tp_name<<"\", only Value";
                    PyRef err(PyObject_CallFunction(PyExc_ValueError, "s", msg.str().c_str()));
                    op->call_cb(err.get());
                    return;
                }
                val = P4PValue_unwrap(temp.get());
                if(val->getStructure()!=structure) {
                    //TODO: attempt safe copy
                    PyRef err(PyObject_CallFunction(PyExc_NotImplementedError, "s", "channelPutConnect() safe copy unimplemneted"));
                    op->call_cb(err.get());
                    return;
                }
            }catch(std::exception& e) {
                PyErr_Print();
                PyErr_Clear();
                std::cerr<<"Error in channelPutConnect value builder: "<<e.what()<<"\n";
                return;
            }
        }
        assert(!!val);
        pvd::BitSet::shared_pointer mask(new pvd::BitSet(1));
        mask->set(0);
        TRACE("send "<<channelPut->getChannel()->getChannelName()<<" mask="<<*mask<<" value="<<val);
        channelPut->lastRequest();
        // may call putDone() recursively
        channelPut->put(val, mask);
        // no going back now...
        op->sent = true;
    }
}

void PutOp::Req::putDone(
    const pvd::Status& status,
    pva::ChannelPut::shared_pointer const & channelPut)
{
    PutOp::shared_pointer op(owner.lock());
    if(!op)
        return;

    PyLock L;
    TRACE("status="<<status);
    if(!op->cb.get()) return;
    PyRef V;

    if(status.isSuccess()) {
        V.reset(Py_None, borrow());
    } else {
        // build Exception instance
        // TODO: create RemoteError type
        V.reset(PyObject_CallFunction(PyExc_RuntimeError, (char*)"s", status.getMessage().c_str()));
    }

    if(!V.get()) {
        PyErr_Print();
        PyErr_Clear();
        std::cerr<<"Error in putDone\n";
    } else {
        op->call_cb(V.get());
    }
}

void RPCOp::restart(const OpBase::shared_pointer& self)
{
    TRACE("channel="<<channel.get()<<" sent="<<sent);
    if(!channel || sent) return;
    pva::ChannelRPC::shared_pointer temp;
    RPCOp::Req::shared_pointer pyreq(new RPCOp::Req(std::tr1::static_pointer_cast<RPCOp>(self)));
    temp.swap(op);
    {
        PyUnlock U;
        if(temp)
            temp->destroy();

        temp = channel->channel->createChannelRPC(pyreq, req);
        TRACE("start RPC "<<temp);
    }
    op = temp;
    channel->ops.insert(self);
}

void RPCOp::lostConn(const OpBase::shared_pointer &self)
{
    if(!channel) return;
    channel->ops.insert(self);
    if(op) {
        pva::ChannelRPC::shared_pointer temp;
        temp.swap(op);

        PyUnlock U;

        temp->destroy();
        temp.reset();
    }
    if(sent) {
        PyRef err(PyObject_CallFunction(PyExc_RuntimeError, "s", "Connection lost before put acknowledged"));
        call_cb(err.get());
    }
}

bool RPCOp::cancel()
{
    bool canceled = OpBase::cancel();
    sent=true;

    if(op) {
        canceled = true;
        pva::ChannelRPC::shared_pointer temp;
        temp.swap(op);

        PyUnlock U;

        temp->destroy();
        temp.reset();
    }

    return canceled;
}

void RPCOp::Req::channelRPCConnect(
    const pvd::Status& status,
    pva::ChannelRPC::shared_pointer const & channelRPC)
{
    RPCOp::shared_pointer op(owner.lock());
    if(!op)
        return;

    TRACE("rpc start "<<channelRPC->getChannel()->getChannelName()<<" "<<status);

    if(!status.isSuccess()) {
        PyLock L;
        PyRef err(PyObject_CallFunction(PyExc_RuntimeError, "s", status.getMessage().c_str()));
        op->call_cb(err.get());
    } else {
        pvd::PVStructure::shared_pointer val;
        {
            PyLock L;
            op->pvvalue.swap(val);
        }

        if(val) {
            channelRPC->lastRequest();
            channelRPC->request(val);
        }
    }
}

void RPCOp::Req::requestDone(
    const pvd::Status& status,
    pva::ChannelRPC::shared_pointer const & channelRPC,
    pvd::PVStructure::shared_pointer const & pvResponse)
{
    RPCOp::shared_pointer op(owner.lock());
    if(!op)
        return;

    TRACE("rpc done "<<channelRPC->getChannel()->getChannelName()<<" "<<status);

    PyLock L;

    if(!status.isSuccess()) {
        PyRef err(PyObject_CallFunction(PyExc_RuntimeError, "s", status.getMessage().c_str()));
        op->call_cb(err.get());
    } else {
        PyRef val(P4PValue_wrap(P4PValue_type, pvResponse));
        op->call_cb(val.get());
    }
}

static PyMethodDef Context_methods[] = {
    {"channel", (PyCFunction)&Context::py_channel, METH_VARARGS|METH_KEYWORDS,
     "Return a Channel"},
    {"close", (PyCFunction)&Context::py_close, METH_NOARGS,
     "Close this Context"},
    {"providers", (PyCFunction)&Context::py_providers, METH_NOARGS|METH_STATIC,
     "Return a list of all currently registered provider names"},
    {"set_debug", (PyCFunction)&Context::py_set_debug, METH_VARARGS|METH_KEYWORDS|METH_STATIC,
     "Set PVA debug level"},
    {"makeRequest", (PyCFunction)&Context::py_makeRequest, METH_VARARGS|METH_STATIC,
     "makeRequest(\"field(value)\")\n\nParse pvRequest string"},
    {NULL}
};

template<>
PyTypeObject PyContext::type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "p4p._p4p.Context",
    sizeof(PyContext),
};

static PyMethodDef Channel_methods[] = {
    {"getName", (PyCFunction)&Channel::py_name, METH_NOARGS,
     "Channel name (aka PV name)"},
    {"get", (PyCFunction)&Channel::py_get, METH_VARARGS|METH_KEYWORDS,
     "get(callback, request=None)\n\nInitiate a new get() operation.\n"
     "The provided callback must be a callable object, which will be called with a single argument.\n"
     "Either a Value or an Exception."},
    {"put", (PyCFunction)&Channel::py_put, METH_VARARGS|METH_KEYWORDS,
     "put(callback, value, request=None)\n\nInitiate a new put() operation.\n"
     "The provided callback must be a callable object, which will be called with a single argument.\n"
     "Either None or an Exception."},
    {"rpc", (PyCFunction)&Channel::py_rpc, METH_VARARGS|METH_KEYWORDS,
     "rpc(callback, value, request=None)\n\nInitiate a new rpc() operation.\n"
     "The provided callback must be a callable object, which will be called with a single argument.\n"
     "Either None or an Exception."},
    {"close", (PyCFunction)&Channel::py_close, METH_NOARGS,
      "close()\n\nDispose of channel."},
    {NULL}
};

template<>
PyTypeObject PyChannel::type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "p4p._p4p.Channel",
    sizeof(PyChannel),
};

static PyMethodDef OpBase_methods[] = {
    {"cancel", (PyCFunction)&OpBase::py_cancel, METH_NOARGS,
     "Cancel pending operation."},
    {NULL}
};

template<>
PyTypeObject PyOp::type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "p4p._p4p.Operation",
    sizeof(PyOp),
};

void unfactory()
{
    pva::ca::CAClientFactory::stop();
    pva::ClientFactory::stop();
}

} // namespace

void p4p_client_register(PyObject *mod)
{
    // TODO: traverse, visit for *Op (with stored PyRef)

    pva::ClientFactory::start();
    pva::ca::CAClientFactory::start();

    Py_AtExit(&unfactory);

    PyContext::buildType();
    PyContext::type.tp_flags = Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE;
    PyContext::type.tp_init = &Context::py_init;

    PyContext::type.tp_methods = Context_methods;

    if(PyType_Ready(&PyContext::type))
        throw std::runtime_error("failed to initialize PyContext");

    Py_INCREF((PyObject*)&PyContext::type);
    if(PyModule_AddObject(mod, "Context", (PyObject*)&PyContext::type)) {
        Py_DECREF((PyObject*)&PyContext::type);
        throw std::runtime_error("failed to add p4p._p4p.Context");
    }


    PyChannel::buildType();
    PyChannel::type.tp_flags = Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE;
    PyChannel::type.tp_init = &Channel::py_init;

    PyChannel::type.tp_methods = Channel_methods;

    if(PyType_Ready(&PyChannel::type))
        throw std::runtime_error("failed to initialize PyChannel");

    Py_INCREF((PyObject*)&PyChannel::type);
    if(PyModule_AddObject(mod, "Channel", (PyObject*)&PyChannel::type)) {
        Py_DECREF((PyObject*)&PyChannel::type);
        throw std::runtime_error("failed to add p4p._p4p.Channel");
    }


    PyOp::buildType();
    PyOp::type.tp_flags = Py_TPFLAGS_DEFAULT|Py_TPFLAGS_HAVE_GC;
    PyOp::type.tp_traverse = &OpBase::py_traverse;
    PyOp::type.tp_clear = &OpBase::py_clear;

    PyOp::type.tp_methods = OpBase_methods;

    if(PyType_Ready(&PyOp::type))
        throw std::runtime_error("failed to initialize PyOp");

    Py_INCREF((PyObject*)&PyOp::type);
    if(PyModule_AddObject(mod, "Operation", (PyObject*)&PyOp::type)) {
        Py_DECREF((PyObject*)&PyOp::type);
        throw std::runtime_error("failed to add p4p._p4p.Operation");
    }

}
