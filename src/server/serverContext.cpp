/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvAccessCPP is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include <epicsSignal.h>

#include <pv/lock.h>
#include <pv/timer.h>
#include <pv/thread.h>

#define epicsExportSharedSymbols
#include <pv/responseHandlers.h>
#include <pv/logger.h>
#include <pv/serverContext.h>
#include <pv/security.h>

using namespace std;
using namespace epics::pvData;
using std::tr1::dynamic_pointer_cast;
using std::tr1::static_pointer_cast;

namespace epics {
namespace pvAccess {

const Version ServerContextImpl::VERSION("pvAccess Server", "cpp",
        EPICS_PVA_MAJOR_VERSION, EPICS_PVA_MINOR_VERSION, EPICS_PVA_MAINTENANCE_VERSION, EPICS_PVA_DEVELOPMENT_FLAG);

ServerContextImpl::ServerContextImpl():
    _beaconAddressList(),
    _ignoreAddressList(),
    _autoBeaconAddressList(true),
    _beaconPeriod(15.0),
    _broadcastPort(PVA_BROADCAST_PORT),
    _serverPort(PVA_SERVER_PORT),
    _receiveBufferSize(MAX_TCP_RECV),
    _timer(),
    _beaconEmitter(),
    _acceptor(),
    _transportRegistry(),
    _channelProviders(),
    _beaconServerStatusProvider(),
    _startTime()
{
    epicsTimeGetCurrent(&_startTime);

    // TODO maybe there is a better place for this (when there will be some factory)
    epicsSignalInstallSigAlarmIgnore ();
    epicsSignalInstallSigPipeIgnore ();

    generateGUID();
    initializeLogger();
}

ServerContextImpl::~ServerContextImpl()
{
    dispose();
}

const GUID& ServerContextImpl::getGUID()
{
    return _guid;
}

const Version& ServerContextImpl::getVersion()
{
    return ServerContextImpl::VERSION;
}

void ServerContextImpl::generateGUID()
{
    // TODO use UUID
    epics::pvData::TimeStamp startupTime;
    startupTime.getCurrent();

    ByteBuffer buffer(_guid.value, sizeof(_guid.value));
    buffer.putLong(startupTime.getSecondsPastEpoch());
    buffer.putInt(startupTime.getNanoseconds());
}

void ServerContextImpl::initializeLogger()
{
    //createFileLogger("serverContextImpl.log");
}

Configuration::const_shared_pointer ServerContextImpl::getConfiguration()
{
    Lock guard(_mutex);
    if (configuration.get() == 0)
    {
        ConfigurationProvider::shared_pointer configurationProvider = ConfigurationFactory::getProvider();
        configuration = configurationProvider->getConfiguration("pvAccess-server");
        if (configuration.get() == 0)
        {
            configuration = configurationProvider->getConfiguration("system");
        }
    }
    return configuration;
}

/**
 * Load configuration.
 */
void ServerContextImpl::loadConfiguration()
{
    Configuration::const_shared_pointer config = configuration;

    // TODO for now just a simple switch
    int32 debugLevel = config->getPropertyAsInteger(PVACCESS_DEBUG, 0);
    if (debugLevel > 0)
        SET_LOG_LEVEL(logLevelDebug);

    // TODO multiple addresses
    _ifaceAddr.ia.sin_family = AF_INET;
    _ifaceAddr.ia.sin_addr.s_addr = htonl(INADDR_ANY);
    _ifaceAddr.ia.sin_port = 0;
    config->getPropertyAsAddress("EPICS_PVAS_INTF_ADDR_LIST", &_ifaceAddr);

    _beaconAddressList = config->getPropertyAsString("EPICS_PVA_ADDR_LIST", _beaconAddressList);
    _beaconAddressList = config->getPropertyAsString("EPICS_PVAS_BEACON_ADDR_LIST", _beaconAddressList);

    _autoBeaconAddressList = config->getPropertyAsBoolean("EPICS_PVA_AUTO_ADDR_LIST", _autoBeaconAddressList);
    _autoBeaconAddressList = config->getPropertyAsBoolean("EPICS_PVAS_AUTO_BEACON_ADDR_LIST", _autoBeaconAddressList);

    _beaconPeriod = config->getPropertyAsFloat("EPICS_PVA_BEACON_PERIOD", _beaconPeriod);
    _beaconPeriod = config->getPropertyAsFloat("EPICS_PVAS_BEACON_PERIOD", _beaconPeriod);

    _serverPort = config->getPropertyAsInteger("EPICS_PVA_SERVER_PORT", _serverPort);
    _serverPort = config->getPropertyAsInteger("EPICS_PVAS_SERVER_PORT", _serverPort);
    _ifaceAddr.ia.sin_port = htons(_serverPort);

    _broadcastPort = config->getPropertyAsInteger("EPICS_PVA_BROADCAST_PORT", _broadcastPort);
    _broadcastPort = config->getPropertyAsInteger("EPICS_PVAS_BROADCAST_PORT", _broadcastPort);

    _receiveBufferSize = config->getPropertyAsInteger("EPICS_PVA_MAX_ARRAY_BYTES", _receiveBufferSize);
    _receiveBufferSize = config->getPropertyAsInteger("EPICS_PVAS_MAX_ARRAY_BYTES", _receiveBufferSize);

    if(_channelProviders.empty()) {
        std::string providers = config->getPropertyAsString("EPICS_PVA_PROVIDER_NAMES", PVACCESS_DEFAULT_PROVIDER);
        providers             = config->getPropertyAsString("EPICS_PVAS_PROVIDER_NAMES", providers);

        ChannelProviderRegistry::shared_pointer reg(getChannelProviderRegistry());

        if (providers == PVACCESS_ALL_PROVIDERS)
        {
            providers.resize(0); // VxWorks 5.5 omits clear()

            std::auto_ptr<ChannelProviderRegistry::stringVector_t> names = reg->getProviderNames();
            for (ChannelProviderRegistry::stringVector_t::iterator iter = names->begin(); iter != names->end(); iter++)
            {
                ChannelProvider::shared_pointer channelProvider = reg->getProvider(*iter);
                if (channelProvider) {
                    _channelProviders.push_back(channelProvider);
                } else {
                    LOG(logLevelDebug, "Provider '%s' all, but missing\n", iter->c_str());
                }
            }

        } else {
            // split space separated names
            std::stringstream ss(providers);
            std::string providerName;
            while (std::getline(ss, providerName, ' '))
            {
                ChannelProvider::shared_pointer channelProvider(reg->getProvider(providerName));
                if (channelProvider) {
                    _channelProviders.push_back(channelProvider);
                } else {
                    LOG(logLevelWarn, "Requested provider '%s' not found", providerName.c_str());
                }
            }
        }
    }

    if(_channelProviders.empty())
        LOG(logLevelError, "ServerContext configured with not Providers will do nothing!\n");

    //
    // introspect network interfaces
    //

    osiSockAttach();

    SOCKET sock = epicsSocketCreate(AF_INET, SOCK_STREAM, 0);
    if (!sock) {
        THROW_BASE_EXCEPTION("Failed to create a socket needed to introspect network interfaces.");
    }

    if (discoverInterfaces(_ifaceList, sock, &_ifaceAddr))
    {
        THROW_BASE_EXCEPTION("Failed to introspect network interfaces.");
    }
    else if (_ifaceList.size() == 0)
    {
        THROW_BASE_EXCEPTION("No (specified) network interface(s) available.");
    }
    epicsSocketDestroy(sock);
}

bool ServerContextImpl::isChannelProviderNamePreconfigured()
{
    Configuration::const_shared_pointer config = getConfiguration();
    return config->hasProperty("EPICS_PVA_PROVIDER_NAMES") || config->hasProperty("EPICS_PVAS_PROVIDER_NAMES");
}

void ServerContextImpl::initialize()
{
    Lock guard(_mutex);

    // already called in loadConfiguration
    //osiSockAttach();

    _timer.reset(new Timer("pvAccess-server timer", lowerPriority));
    _transportRegistry.reset(new TransportRegistry());

    ServerContextImpl::shared_pointer thisServerContext = shared_from_this();
    _responseHandler.reset(new ServerResponseHandler(thisServerContext));

    _acceptor.reset(new BlockingTCPAcceptor(thisServerContext, _responseHandler, _ifaceAddr, _receiveBufferSize));
    _serverPort = ntohs(_acceptor->getBindAddress()->ia.sin_port);

    // setup broadcast UDP transport
    initializeUDPTransports(true, _udpTransports, _ifaceList, _responseHandler, _broadcastTransport,
                            _broadcastPort, _autoBeaconAddressList, _beaconAddressList, _ignoreAddressList);

    _beaconEmitter.reset(new BeaconEmitter("tcp", _broadcastTransport, thisServerContext));

    _beaconEmitter->start();
}

void ServerContextImpl::run(uint32 seconds)
{
    //TODO review this
    if(seconds == 0)
    {
        _runEvent.wait();
    }
    else
    {
        _runEvent.wait(seconds);
    }
}

void ServerContextImpl::shutdown()
{
    // stop responding to search requests
    for (BlockingUDPTransportVector::const_iterator iter = _udpTransports.begin();
            iter != _udpTransports.end(); iter++)
        (*iter)->close();
    _udpTransports.clear();

    // stop emitting beacons
    if (_beaconEmitter)
    {
        _beaconEmitter->destroy();
        _beaconEmitter.reset();
    }

    // close UDP sent transport
    if (_broadcastTransport)
    {
        _broadcastTransport->close();
        _broadcastTransport.reset();
    }

    // stop accepting connections
    if (_acceptor)
    {
        _acceptor->destroy();
        _acceptor.reset();
    }

    // this will also destroy all channels
    destroyAllTransports();

    // response handlers hold strong references to us,
    // so must break the cycles
    _responseHandler.reset();

    _runEvent.signal();
}

void ServerContextImpl::destroyAllTransports()
{

    // not initialized yet
    if (!_transportRegistry.get())
    {
        return;
    }

    std::auto_ptr<TransportRegistry::transportVector_t> transports = _transportRegistry->toArray();
    if (transports.get() == 0)
        return;

    int size = (int)transports->size();
    if (size == 0)
        return;

    LOG(logLevelInfo, "Server context still has %d transport(s) active and closing...", size);

    for (int i = 0; i < size; i++)
    {
        Transport::shared_pointer transport = (*transports)[i];
        try
        {
            transport->close();
        }
        catch (std::exception &e)
        {
            // do all exception safe, log in case of an error
            LOG(logLevelError, "Unhandled exception caught from client code at %s:%d: %s", __FILE__, __LINE__, e.what());
        }
        catch (...)
        {
            // do all exception safe, log in case of an error
            LOG(logLevelError, "Unhandled exception caught from client code at %s:%d.", __FILE__, __LINE__);
        }
    }

    // now clear all (release)
    _transportRegistry->clear();

}

void ServerContext::printInfo()
{
    printInfo(cout);
}

void ServerContextImpl::printInfo(ostream& str)
{
    Lock guard(_mutex);
    str << "VERSION : " << getVersion().getVersionString() << endl
        << "PROVIDER_NAMES : ";
    for(std::vector<ChannelProvider::shared_pointer>::const_iterator it = _channelProviders.begin();
        it != _channelProviders.end(); ++it)
    {
        str<<(*it)->getProviderName()<<", ";
    }
    str << endl
        << "BEACON_ADDR_LIST : " << _beaconAddressList << endl
        << "AUTO_BEACON_ADDR_LIST : " << _autoBeaconAddressList << endl
        << "BEACON_PERIOD : " << _beaconPeriod << endl
        << "BROADCAST_PORT : " << _broadcastPort << endl
        << "SERVER_PORT : " << _serverPort << endl
        << "RCV_BUFFER_SIZE : " << _receiveBufferSize << endl
        << "IGNORE_ADDR_LIST: " << _ignoreAddressList << endl
        << "INTF_ADDR_LIST : " << inetAddressToString(_ifaceAddr, false) << endl;
}

void ServerContext::dispose()
{
    try
    {
        shutdown();
    }
    catch(std::exception& e)
    {
        std::cerr<<"Error in: ServerContextImpl::dispose: "<<e.what()<<"\n";
    }
    catch(...)
    {
        std::cerr<<"Oh no, something when wrong in ServerContextImpl::dispose!\n";
    }
}

void ServerContextImpl::setBeaconServerStatusProvider(BeaconServerStatusProvider::shared_pointer const & beaconServerStatusProvider)
{
    _beaconServerStatusProvider = beaconServerStatusProvider;
}

std::string ServerContextImpl::getBeaconAddressList()
{
    return _beaconAddressList;
}

bool ServerContextImpl::isAutoBeaconAddressList()
{
    return _autoBeaconAddressList;
}

float ServerContextImpl::getBeaconPeriod()
{
    return _beaconPeriod;
}

int32 ServerContextImpl::getReceiveBufferSize()
{
    return _receiveBufferSize;
}

int32 ServerContextImpl::getServerPort()
{
    return _serverPort;
}

int32 ServerContextImpl::getBroadcastPort()
{
    return _broadcastPort;
}

std::string ServerContextImpl::getIgnoreAddressList()
{
    return _ignoreAddressList;
}

BeaconServerStatusProvider::shared_pointer ServerContextImpl::getBeaconServerStatusProvider()
{
    return _beaconServerStatusProvider;
}

osiSockAddr* ServerContextImpl::getServerInetAddress()
{
    if(_acceptor.get())
    {
        return const_cast<osiSockAddr*>(_acceptor->getBindAddress());
    }
    return NULL;
}

BlockingUDPTransport::shared_pointer ServerContextImpl::getBroadcastTransport()
{
    return _broadcastTransport;
}

std::vector<ChannelProvider::shared_pointer>& ServerContextImpl::getChannelProviders()
{
    return _channelProviders;
}

Timer::shared_pointer ServerContextImpl::getTimer()
{
    return _timer;
}

TransportRegistry::shared_pointer ServerContextImpl::getTransportRegistry()
{
    return _transportRegistry;
}

Channel::shared_pointer ServerContextImpl::getChannel(pvAccessID /*id*/)
{
    // not used
    return Channel::shared_pointer();
}

Transport::shared_pointer ServerContextImpl::getSearchTransport()
{
    // not used
    return Transport::shared_pointer();
}

void ServerContextImpl::newServerDetected()
{
    // not used
}

epicsTimeStamp& ServerContextImpl::getStartTime()
{
    return _startTime;
}


std::map<std::string, std::tr1::shared_ptr<SecurityPlugin> >& ServerContextImpl::getSecurityPlugins()
{
    return SecurityPluginRegistry::instance().getServerSecurityPlugins();
}



ServerContext::shared_pointer startPVAServer(std::string const & providerNames, int timeToRun, bool runInSeparateThread, bool printInfo)
{
    ServerContext::shared_pointer ret(ServerContext::create(ServerContext::Config()
                                 .config(ConfigurationBuilder()
                                         .add("EPICS_PVAS_PROVIDER_NAMES", providerNames)
                                         .push_map()
                                         .push_env() // environment takes precidence (top of stack)
                                         .build())));
    if(printInfo)
        ret->printInfo();

    if(!runInSeparateThread) {
        ret->run(timeToRun);
        ret->shutdown();
    } else if(timeToRun!=0) {
        LOG(logLevelWarn, "startPVAServer() timeToRun!=0 only supported when runInSeparateThread==false\n");
    }

    return ret;
}

namespace {
struct shutdown_dtor {
    ServerContextImpl::shared_pointer wrapped;
    shutdown_dtor(const ServerContextImpl::shared_pointer& wrapped) :wrapped(wrapped) {}
    void operator()(ServerContext* self) {
        wrapped->shutdown();
        if(!wrapped.unique())
            LOG(logLevelWarn, "ServerContextImpl::shutdown() doesn't break all internal ref. loops. use_count=%u\n", (unsigned)wrapped.use_count());
        wrapped.reset();
    }
};
}

ServerContext::shared_pointer ServerContext::create(const Config &conf)
{
    ServerContextImpl::shared_pointer ret(new ServerContextImpl());
    ret->configuration = conf._conf;
    ret->_channelProviders = conf._providers;

    if (!ret->configuration)
    {
        ConfigurationProvider::shared_pointer configurationProvider = ConfigurationFactory::getProvider();
        ret->configuration = configurationProvider->getConfiguration("pvAccess-server");
        if (!ret->configuration)
        {
            ret->configuration = configurationProvider->getConfiguration("system");
        }
    }
    if(!ret->configuration) {
        ret->configuration = ConfigurationBuilder().push_env().build();
    }

    ret->loadConfiguration();
    ret->initialize();

    // wrap the returned shared_ptr so that it's dtor calls ->shutdown() to break internal referance loops
    {
        ServerContextImpl::shared_pointer wrapper(ret.get(), shutdown_dtor(ret));
        wrapper.swap(ret);
    }

    return ret;
}

}}
