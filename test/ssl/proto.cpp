#include <iostream>
#include <string>
#include <sstream>
#include <deque>
#include <algorithm>
#include <cstring>
#include <limits>

// Unit test for OpenVPN protocol

#define OPENVPN_DEBUG
#define USE_TLS_AUTH

// number of threads to use for test
#ifndef N_THREADS
#define N_THREADS 1
#endif

// number of iterations
#ifndef ITER
#define ITER 1000000
#endif

// number of high-level session iterations
#ifndef SITER
#define SITER 1
#endif

// abort if we reach this limit
//#define DROUGHT_LIMIT 100000

#if !defined(VERBOSE) && ITER <= 10000
#define VERBOSE
#endif

#include <openvpn/common/thread.hpp>
#include <openvpn/common/exception.hpp>
#include <openvpn/common/file.hpp>
#include <openvpn/time/time.hpp>
#include <openvpn/random/rand.hpp>
#include <openvpn/frame/frame.hpp>
#include <openvpn/ssl/proto.hpp>

#include <openvpn/openssl/ssl/sslctx.hpp>
#include <openvpn/openssl/util/init.hpp>

#ifdef USE_APPLE_SSL
#include <openvpn/applecrypto/ssl/sslctx.hpp>
#endif

#ifdef OPENSSL_AES_NI
#include <openvpn/openssl/util/engine.hpp>
#endif

#if OPENVPN_MULTITHREAD
#include <boost/bind.hpp>
#endif

using namespace openvpn;

// server SSL implementation is always OpenSSL-based
typedef OpenSSLContext ServerSSLContext;

// client SSL implementation can be OpenSSL or Apple SSL
#ifdef USE_APPLE_SSL
typedef AppleSSLContext ClientSSLContext;
#else
typedef OpenSSLContext ClientSSLContext;
#endif

const char message[] =
  "Message _->_ 0000000000 It was a bright cold day in April, and the clocks\n"
  "were striking thirteen. Winston Smith, his chin nuzzled\n"
  "into his breast in an effort to escape the vile wind,\n"
  "slipped quickly through the glass doors of Victory\n"
  "Mansions, though not quickly enough to prevent a\n"
  "swirl of gritty dust from entering along with him.\n"
#ifdef LARGE_MESSAGE
  "It was a bright cold day in April, and the clocks\n"
  "were striking thirteen. Winston Smith, his chin nuzzled\n"
  "into his breast in an effort to escape the vile wind,\n"
  "slipped quickly through the glass doors of Victory\n"
  "Mansions, though not quickly enough to prevent a\n"
  "swirl of gritty dust from entering along with him.\n"
  "It was a bright cold day in April, and the clocks\n"
  "were striking thirteen. Winston Smith, his chin nuzzled\n"
  "into his breast in an effort to escape the vile wind,\n"
  "slipped quickly through the glass doors of Victory\n"
  "Mansions, though not quickly enough to prevent a\n"
  "swirl of gritty dust from entering along with him.\n"
  "It was a bright cold day in April, and the clocks\n"
  "were striking thirteen. Winston Smith, his chin nuzzled\n"
  "into his breast in an effort to escape the vile wind,\n"
  "slipped quickly through the glass doors of Victory\n"
  "Mansions, though not quickly enough to prevent a\n"
  "swirl of gritty dust from entering along with him.\n"
  "It was a bright cold day in April, and the clocks\n"
  "were striking thirteen. Winston Smith, his chin nuzzled\n"
  "into his breast in an effort to escape the vile wind,\n"
  "slipped quickly through the glass doors of Victory\n"
  "Mansions, though not quickly enough to prevent a\n"
  "swirl of gritty dust from entering along with him.\n"
#endif
  ;

// A "Drought" measures the maximum period of time between
// any two successive events.  Used to measure worst-case
// packet loss.
class DroughtMeasure
{
public:
  OPENVPN_SIMPLE_EXCEPTION(drought_limit_exceeded);

  DroughtMeasure(const std::string& name_arg, TimePtr now_arg)
    : now(now_arg), name(name_arg)
  {
  }

  void event()
  {
    if (last_event.defined())
      {
	Time::Duration since_last = *now - last_event;
	if (since_last > drought)
	  {
	    drought = since_last;
#if defined(VERBOSE) || defined(DROUGHT_LIMIT)
	    {
	      const unsigned int r = drought.raw();
#ifdef VERBOSE
	      std::cout << "*** Drought " << name << " has reached " << r << std::endl;
#endif
#ifdef DROUGHT_LIMIT
	      if (r > DROUGHT_LIMIT)
		throw drought_limit_exceeded();
#endif
	    }
#endif
	  }
      }
    last_event = *now;
  }

  Time::Duration operator()() const { return drought; }

private:
  TimePtr now;
  Time last_event;
  Time::Duration drought;
  std::string name;
};

// test the OpenVPN protocol implementation in ProtoContext
template <typename SSL_CONTEXT>
class TestProto : public ProtoContext<SSL_CONTEXT>
{
  typedef ProtoContext<SSL_CONTEXT> Base;

  using Base::now;
  using Base::mode;
  using Base::is_server;

public:
  using Base::flush;

  typedef typename Base::PacketType PacketType;

  TestProto(const typename Base::Config::Ptr& config,
	    const ProtoStats::Ptr& stats)
    : Base(config, stats),
      control_drought("control", config->now),
      data_drought("data", config->now),
      frame(config->frame),
      app_bytes_(0),
      net_bytes_(0),
      data_bytes_(0)
  {
    // zero progress value
    std::memset(progress_, 0, 11);
  }

  void reset()
  {
    net_out.clear();
    Base::reset();
  }

  void initial_app_send(const char *msg)
  {
    Base::start();

    const size_t msglen = std::strlen(msg);
    BufferAllocated app_buf((unsigned char *)msg, msglen, 0);
    copy_progress(app_buf);
    control_send(app_buf);
    flush(true);
  }

  bool do_housekeeping()
  {
    if (now() >= Base::next_housekeeping())
      {
	Base::housekeeping();
	return true;
      }
    else
      return false;
  }

  void control_send(BufferPtr& app_bp)
  {
    app_bytes_ += app_bp->size();
    Base::control_send(app_bp);
  }

  void control_send(BufferAllocated& app_buf)
  {
    app_bytes_ += app_buf.size();
    Base::control_send(app_buf);
  }

  BufferPtr data_encrypt_string(const char *str)
  {
    BufferPtr bp = new BufferAllocated();
    frame->prepare(Frame::READ_LINK_UDP, *bp);
    bp->write((unsigned char *)str, std::strlen(str));
    data_encrypt(*bp);
    return bp;
  }

  void data_encrypt(BufferAllocated& in_out)
  {
    Base::data_encrypt(in_out);
  }

  void data_decrypt(const PacketType& type, BufferAllocated& in_out)
  {
    Base::data_decrypt(type, in_out);
    if (in_out.size())
      {
	data_bytes_ += in_out.size();
	data_drought.event();
      }
  }

  size_t net_bytes() const { return net_bytes_; }
  size_t app_bytes() const { return app_bytes_; }
  size_t data_bytes() const { return data_bytes_; }

  const char *progress() const { return progress_; }

  void finalize()
  {
    data_drought.event();
    control_drought.event();
  }

  std::deque<BufferPtr> net_out;

  DroughtMeasure control_drought;
  DroughtMeasure data_drought;

private:
  virtual void control_net_send(const Buffer& net_buf)
  {
    net_bytes_ += net_buf.size();
    net_out.push_back(BufferPtr(new BufferAllocated(net_buf, 0)));
  }

  virtual void control_recv(BufferPtr& app_bp)
  {
    BufferPtr work;
    work.swap(app_bp);
    if (work->size() >= 23)
      std::memcpy(progress_, work->data()+13, 10);

#ifdef VERBOSE
    {
      const ssize_t trunc = 64;
      const std::string show((char *)work->data(), trunc);
      std::cout << now().raw() << " " << mode().str() << " " << show << std::endl;
    }
#endif
    modmsg(work);
    control_send(work);
    control_drought.event();
  }

  void copy_progress(Buffer& buf)
  {
    if (progress_[0]) // make sure progress was initialized
      std::memcpy(buf.data()+13, progress_, 10);    
  }

  void modmsg(BufferPtr& buf)
  {
    char *msg = (char *) buf->data();
    if (is_server())
      {
	msg[8] = 'S';
	msg[11] = 'C';
      }
    else
      {
	msg[8] = 'C';
	msg[11] = 'S';
      }

    // increment embedded number
    for (int i = 22; i >= 13; i--)
      {
	if (msg[i] != '9')
	  {
	    msg[i]++;
	    break;
	  }
	else
	  msg[i] = '0';
      }
  }

  Frame::Ptr frame;
  size_t app_bytes_;
  size_t net_bytes_;
  size_t data_bytes_;
  char progress_[11];
};

template <typename SSL_CONTEXT>
class TestProtoClient : public TestProto<SSL_CONTEXT>
{
  typedef TestProto<SSL_CONTEXT> Base;
public:
  TestProtoClient(const typename Base::Config::Ptr& config,
		  const ProtoStats::Ptr& stats)
    : Base(config, stats)
  {
  }

private:
  virtual void client_auth(Buffer& buf)
  {
    const std::string username("foo");
    const std::string password("bar");
    Base::write_auth_string(username, buf);
    Base::write_auth_string(password, buf);
  }
};

template <typename SSL_CONTEXT>
class TestProtoServer : public TestProto<SSL_CONTEXT>
{
  typedef TestProto<SSL_CONTEXT> Base;
public:
  OPENVPN_SIMPLE_EXCEPTION(auth_failed);

  TestProtoServer(const typename Base::Config::Ptr& config,
		  const ProtoStats::Ptr& stats)
    : Base(config, stats)
  {
  }

private:
  virtual void server_auth(Buffer& buf, const std::string& peer_info)
  {
    const std::string username = Base::template read_auth_string<std::string>(buf);
    const std::string password = Base::template read_auth_string<std::string>(buf);

#ifdef VERBOSE
    std::cout << "**** AUTHENTICATE " << username << '/' << password << " PEER INFO:" << std::endl;
    std::cout << peer_info;
#endif
    if (username != "foo" || password != "bar")
      throw auth_failed();
  }
};

// Simulate a noisy transmission channel where packets can be dropped,
// reordered, or corrupted.
class NoisyWire
{
public:
  OPENVPN_SIMPLE_EXCEPTION(session_invalidated);

  NoisyWire(const std::string title_arg,
	    TimePtr now_arg,
	    RandomIntBase& rand_arg,
	    const unsigned int reorder_prob_arg,
	    const unsigned int drop_prob_arg,
	    const unsigned int corrupt_prob_arg)
    : title(title_arg),
      now(now_arg),
      random(rand_arg),
      reorder_prob(reorder_prob_arg),
      drop_prob(drop_prob_arg),
      corrupt_prob(corrupt_prob_arg)
  {
  }

  template <typename T1, typename T2>
  void xfer(T1& a, T2& b)
  {
    // check for errors
    if (a.invalidated() || b.invalidated())
	throw session_invalidated();

    // need to retransmit?
    if (a.do_housekeeping())
      {
#ifdef VERBOSE
	std::cout << now->raw() << " " << title << " Housekeeping" << std::endl;
#endif
      }

    // queue a data channel packet
    if (a.data_channel_ready())
      {
	BufferPtr bp = a.data_encrypt_string("Waiting for godot...");
	wire.push_back(bp);
      }

    // transfer network packets from A -> wire
    while (!a.net_out.empty())
      {
	BufferPtr bp = a.net_out.front();
#ifdef VERBOSE
	std::cout << now->raw() << " " << title << " " << a.dump_packet(*bp) <<  std::endl;
#endif
	a.net_out.pop_front();
	wire.push_back(bp);
      }

    // transfer network packets from wire -> B
    while (true)
      {
	BufferPtr bp = recv();
	if (!bp)
	  break;
	typename T2::PacketType pt = b.packet_type(*bp);
	if (pt.is_control())
	  {
#ifdef VERBOSE
	    if (!b.control_net_validate(pt, *bp)) // not strictly necessary since control_net_recv will also validate
	      std::cout << now->raw() << " " << title << " CONTROL PACKET VALIDATION FAILED" << std::endl;
#endif
	    b.control_net_recv(pt, bp);
	  }
	else if (pt.is_data())
	  {
	    try {
	      b.data_decrypt(pt, *bp);
#ifdef VERBOSE
	      if (bp->size())
		{
		  const std::string show((char *)bp->data(), bp->size());
		  std::cout << now->raw() << " " << title << " DATA CHANNEL DECRYPT: " << show << std::endl;
		}
#endif
	    }
	    catch (std::exception& e)
	      {
#ifdef VERBOSE
		std::cout << now->raw() << " " << title << " Exception on data channel decrypt: " << e.what() << std::endl;
#endif
	      }
	  }
      }
    b.flush(true);
  }

private:
  BufferPtr recv()
  {
    // simulate packets being received out of order
    if (wire.size() >= 2 && !rand(reorder_prob))
      {
	const size_t i = random.randrange(wire.size() - 1) + 1;
#ifdef VERBOSE
	std::cout << now->raw() << " " << title << " Simulating packet reordering " << i << " -> 0" <<  std::endl;
#endif
	std::swap(wire[0], wire[i]);
      }

    if (wire.size())
      {
	BufferPtr bp = wire.front();
	wire.pop_front();

#ifdef VERBOSE
	std::cout << now->raw() << " " << title << " Received packet, size=" << bp->size() << std::endl;
#endif

	// simulate dropped packet
	if (!rand(drop_prob))
	  {
#ifdef VERBOSE
	    std::cout << now->raw() << " " << title << " Simulating a dropped packet" << std::endl;
#endif
	    return BufferPtr();
	  }

	// simulate corrupted packet
	if (bp->size() && !rand(corrupt_prob))
	  {
#ifdef VERBOSE
	    std::cout << now->raw() << " " << title << " Simulating a corrupted packet" << std::endl;
#endif
	    const size_t pos = random.randrange(bp->size());
	    const unsigned char value = random.randrange(256);
	    (*bp)[pos] = value;
	  }
	return bp;
      }

    return BufferPtr();
  }

  unsigned int rand(const unsigned int prob)
  {
    if (prob)
      return random.randrange(prob);
    else
      return 1;
  }
  
  std::string title;
  TimePtr now;
  RandomIntBase& random;
  unsigned int reorder_prob;
  unsigned int drop_prob;
  unsigned int corrupt_prob;
  std::deque<BufferPtr> wire;
};

// execute the unit test in one thread
void test(const int thread_num)
{
  try {
    // frame
    Frame::Ptr frame(new Frame(Frame::Context(128, 256, 128, 0, 16, 0)));

    // RNG
    RandomInt rand;
    PRNG::Ptr prng(new PRNG("sha1", 16));

    // init simulated time
    Time time;
    const Time::Duration time_step = Time::Duration::binary_ms(100);

    // client config files
    const std::string ca1_crt = read_text("ca1.crt");
    const std::string ca2_crt = read_text("ca2.crt");
    const std::string client_crt = read_text("client.crt");
    const std::string client_key = read_text("client.key");
    const std::string server_crt = read_text("server.crt");
    const std::string server_key = read_text("server.key");
    const std::string dh_pem = read_text("dh.pem");
    const std::string tls_auth_key = read_text("tls-auth.key");

    // client config
    ClientSSLContext::Config cc;
    cc.mode = Mode(Mode::CLIENT);
    cc.frame = frame;
#ifdef USE_APPLE_SSL
    cc.identity = "etest";
#else
    cc.load_ca(ca1_crt + ca2_crt);
    cc.load_cert(client_crt);
    cc.load_private_key(client_key);
#endif
#ifdef VERBOSE
    cc.enable_debug();
#endif

    // client stats
    ProtoStats::Ptr cli_stats(new ProtoStats);

    // client ProtoContext config
    typedef ProtoContext<ClientSSLContext> ClientProtoContext;
    ClientProtoContext::Config::Ptr cp(new ClientProtoContext::Config);
    cp->ssl_ctx.reset(new ClientSSLContext(cc));
    cp->frame = frame;
    cp->now = &time;
    cp->prng = prng;
    cp->protocol = Protocol(Protocol::UDPv4);
    cp->layer = Layer(Layer::OSI_LAYER_3);
    cp->comp_ctx = CompressContext(CompressContext::LZO_STUB);
    cp->cipher = Cipher("AES-128-CBC");
    cp->digest = Digest("SHA1");
#ifdef USE_TLS_AUTH
    cp->tls_auth_key.parse(tls_auth_key);
    cp->tls_auth_digest = Digest("sha1");
#endif
    cp->reliable_window = 4;
    cp->max_ack_list = 4;
    cp->pid_mode = PacketIDReceive::UDP_MODE;
    cp->pid_seq_backtrack = 64;
    cp->pid_time_backtrack = 30;
    cp->pid_debug_level = PacketIDReceive::DEBUG_QUIET;
#if SITER > 1
    cp->handshake_window = Time::Duration::seconds(30);
#else
    cp->handshake_window = Time::Duration::seconds(18); // will cause a small number of handshake failures
#endif
    cp->become_primary = Time::Duration::seconds(30);
    cp->renegotiate = Time::Duration::seconds(95);
    cp->expire = Time::Duration::seconds(150);
    cp->keepalive_ping = Time::Duration::seconds(5);
    cp->keepalive_timeout = Time::Duration::seconds(60);

#ifdef VERBOSE
    std::cout << "CLIENT OPTIONS: " << cp->options_string() << std::endl;
    std::cout << "CLIENT PEER INFO:" << std::endl;
    std::cout << cp->peer_info_string();
#endif

    // server config
    ServerSSLContext::Config sc;
    sc.mode = Mode(Mode::SERVER);
    sc.frame = frame;
    sc.load_ca(ca1_crt + ca2_crt);
    sc.load_cert(server_crt);
    sc.load_private_key(server_key);
    sc.load_dh(dh_pem);
#ifdef VERBOSE
    sc.enable_debug();
#endif

    // server ProtoContext config
    typedef ProtoContext<ServerSSLContext> ServerProtoContext;
    ServerProtoContext::Config::Ptr sp(new ServerProtoContext::Config);
    sp->ssl_ctx.reset(new ServerSSLContext(sc));
    sp->frame = frame;
    sp->now = &time;
    sp->prng = prng;
    sp->protocol = Protocol(Protocol::UDPv4);
    sp->layer = Layer(Layer::OSI_LAYER_3);
    sp->comp_ctx = CompressContext(CompressContext::LZO_STUB);
    sp->cipher = Cipher("AES-128-CBC");
    sp->digest = Digest("SHA1");
#ifdef USE_TLS_AUTH
    sp->tls_auth_key.parse(tls_auth_key);
    sp->tls_auth_digest = Digest("sha1");
#endif
    sp->reliable_window = 4;
    sp->max_ack_list = 4;
    sp->pid_mode = PacketIDReceive::UDP_MODE;
    sp->pid_seq_backtrack = 64;
    sp->pid_time_backtrack = 30;
    sp->pid_debug_level = PacketIDReceive::DEBUG_QUIET;
#if SITER > 1
    sp->handshake_window = Time::Duration::seconds(30);
#else
    sp->handshake_window = Time::Duration::seconds(17) + Time::Duration::binary_ms(512);
#endif
    sp->become_primary = Time::Duration::seconds(30);
    sp->renegotiate = Time::Duration::seconds(90);
    sp->expire = Time::Duration::seconds(150);
    sp->keepalive_ping = Time::Duration::seconds(5);
    sp->keepalive_timeout = Time::Duration::seconds(60);

#ifdef VERBOSE
    std::cout << "SERVER OPTIONS: " << sp->options_string() << std::endl;
    std::cout << "SERVER PEER INFO:" << std::endl;
    std::cout << sp->peer_info_string();
#endif

    // server stats
    ProtoStats::Ptr serv_stats(new ProtoStats);

    TestProtoClient<ClientSSLContext> cli_proto(cp, cli_stats);
    TestProtoServer<OpenSSLContext> serv_proto(sp, serv_stats);

    for (int i = 0; i < SITER; ++i)
      {
#ifdef VERBOSE
	std::cout << "***** SITER " << i << std::endl;
#endif
	cli_proto.reset();
	serv_proto.reset();

	NoisyWire client_to_server("Client -> Server", &time, rand, 8, 16, 32); // last value: 32
	NoisyWire server_to_client("Server -> Client", &time, rand, 8, 16, 32); // last value: 32

	// start feedback loop
	cli_proto.initial_app_send(message);
	serv_proto.start();

	// message loop
	for (int j = 0; j < ITER; ++j)
	  {
	    client_to_server.xfer(cli_proto, serv_proto);
	    server_to_client.xfer(serv_proto, cli_proto);
	    time += time_step;
	  }
      }

    cli_proto.finalize();
    serv_proto.finalize();

    const size_t ab = cli_proto.app_bytes() + serv_proto.app_bytes();
    const size_t nb = cli_proto.net_bytes() + serv_proto.net_bytes();
    const size_t db = cli_proto.data_bytes() + serv_proto.data_bytes();

    std::cout << "*** app bytes=" << ab
	      << " net_bytes=" << nb
	      << " data_bytes=" << db
	      << " prog=" << cli_proto.progress() << '/' << serv_proto.progress()
              << " D=" << cli_proto.control_drought().raw() << '/' << cli_proto.data_drought().raw() << '/' << serv_proto.control_drought().raw() << '/' << serv_proto.data_drought().raw()
              << " N=" << cli_proto.negotiations() << '/' << serv_proto.negotiations()
              << " SH=" << cli_proto.slowest_handshake().raw() << '/' << serv_proto.slowest_handshake().raw()
              << " HE=" << cli_stats->get(ProtoStats::HANDSHAKE_TIMEOUT) << '/' << serv_stats->get(ProtoStats::HANDSHAKE_TIMEOUT)
	      << std::endl;
  }
  catch (std::exception& e)
    {
      std::cerr << "Exception: " << e.what() << std::endl;
    }
}

int main(int /*argc*/, char* /*argv*/[])
{
  Time::reset_base();
  openssl_init ossl_init;

#ifdef OPENSSL_AES_NI
  openssl_setup_engine("aesni");
#endif

#if N_THREADS >= 2 && OPENVPN_MULTITHREAD
  boost::thread* threads[N_THREADS];
  size_t i;
  for (i = 0; i < N_THREADS; ++i)
    {
      threads[i] = new boost::thread(boost::bind(&test, i));
    }
  for (i = 0; i < N_THREADS; ++i)
    {
      threads[i]->join();
      delete threads[i];
    }
#else
  test(1);
#endif
  return 0;
}
