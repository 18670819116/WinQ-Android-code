//    OpenVPN -- An application to securely tunnel IP networks
//               over a single port, with support for SSL/TLS-based
//               session authentication and key exchange,
//               packet encryption, packet authentication, and
//               packet compression.
//
//    Copyright (C) 2012-2017 OpenVPN Inc.
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU Affero General Public License Version 3
//    as published by the Free Software Foundation.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU Affero General Public License for more details.
//
//    You should have received a copy of the GNU Affero General Public License
//    along with this program in the COPYING file.
//    If not, see <http://www.gnu.org/licenses/>.

// UDP transport object specialized for client.

#ifndef OPENVPN_TRANSPORT_CLIENT_UDPCLI_H
#define OPENVPN_TRANSPORT_CLIENT_UDPCLI_H

#include <sstream>

#include <openvpn/io/io.hpp>

#include <openvpn/common/likely.hpp>
#include <openvpn/common/platform.hpp>
#include <openvpn/transport/udplink.hpp>
#include <openvpn/transport/client/transbase.hpp>
#include <openvpn/transport/socket_protect.hpp>
#include <openvpn/client/remotelist.hpp>

namespace openvpn {
  namespace UDPTransport {

    class ClientConfig : public TransportClientFactory
    {
    public:
      typedef RCPtr<ClientConfig> Ptr;

      RemoteList::Ptr remote_list;
      bool server_addr_float;
      bool synchronous_dns_lookup;
      int n_parallel;
      Frame::Ptr frame;
      SessionStats::Ptr stats;

      SocketProtect* socket_protect;

#ifdef OPENVPN_GREMLIN
      Gremlin::Config::Ptr gremlin_config;
#endif

      static Ptr new_obj()
      {
	return new ClientConfig;
      }

      virtual TransportClient::Ptr new_transport_client_obj(openvpn_io::io_context& io_context,
							    TransportClientParent* parent);

    private:
      ClientConfig()
	: server_addr_float(false),
	  synchronous_dns_lookup(false),
	  n_parallel(8),
	  socket_protect(nullptr)
      {}
    };

    class Client : public TransportClient
    {
      typedef RCPtr<Client> Ptr;

      friend class ClientConfig;  // calls constructor
      friend class Link<Client*>; // calls udp_read_handler

      typedef Link<Client*> LinkImpl;

    public:
      virtual void transport_start()
      {
	if (!impl)
	  {
	    halt = false;
	    if (config->remote_list->endpoint_available(&server_host, &server_port, nullptr))
	      {
		start_connect_();
	      }
	    else
	      {
		parent->transport_pre_resolve();

		if (config->synchronous_dns_lookup)
		  {
		    openvpn_io::error_code error;
		    openvpn_io::ip::udp::resolver::results_type results = resolver.resolve(server_host, server_port, error);
		    do_resolve_(error, results);
		  }
		else
		  {
		    resolver.async_resolve(server_host, server_port,
					   [self=Ptr(this)](const openvpn_io::error_code& error, openvpn_io::ip::udp::resolver::results_type results)
					   {
					     self->do_resolve_(error, results);
					   });
		  }
	      }
	  }
      }

      virtual bool transport_send_const(const Buffer& buf)
      {
	return send(buf);
      }

      virtual bool transport_send(BufferAllocated& buf)
      {
	return send(buf);
      }

      virtual bool transport_send_queue_empty() // really only has meaning for TCP
      {
	return false;
      }

      virtual bool transport_has_send_queue()
      {
	return false;
      }

      virtual void transport_stop_requeueing() { }

      virtual unsigned int transport_send_queue_size()
      {
	return 0;
      }

      virtual void reset_align_adjust(const size_t align_adjust)
      {
	if (impl)
	  impl->reset_align_adjust(align_adjust);
      }

      virtual void server_endpoint_info(std::string& host, std::string& port, std::string& proto, std::string& ip_addr) const
      {
	host = server_host;
	port = server_port;
	const IP::Addr addr = server_endpoint_addr();
	proto = "UDP";
	proto += addr.version_string();
	ip_addr = addr.to_string();
      }

      virtual IP::Addr server_endpoint_addr() const
      {
	return IP::Addr::from_asio(server_endpoint.address());
      }

      virtual Protocol transport_protocol() const
      {
	if (server_endpoint.address().is_v4())
	  return Protocol(Protocol::UDPv4);
	else if (server_endpoint.address().is_v6())
	  return Protocol(Protocol::UDPv6);
	else
	  return Protocol();
      }

      virtual void stop() { stop_(); }
      virtual ~Client() { stop_(); }

    private:
      Client(openvpn_io::io_context& io_context_arg,
	     ClientConfig* config_arg,
	     TransportClientParent* parent_arg)
	:  io_context(io_context_arg),
	   socket(io_context_arg),
	   config(config_arg),
	   parent(parent_arg),
	   resolver(io_context_arg),
	   halt(false)
      {
      }

      virtual void transport_reparent(TransportClientParent* parent_arg)
      {
	parent = parent_arg;
      }

      bool send(const Buffer& buf)
      {
	if (impl)
	  {
	    const int err = impl->send(buf, nullptr);
	    if (unlikely(err))
	      {
		// While UDP errors are generally ignored, certain
		// errors should be forwarded up to the higher levels.
#ifdef OPENVPN_PLATFORM_IPHONE
		if (err == EADDRNOTAVAIL)
		  {
		    stop();
		    parent->transport_error(Error::TRANSPORT_ERROR, "EADDRNOTAVAIL: Can't assign requested address");
		  }
#endif
		return false;
	      }
	    else
	      return true;
	  }
	else
	  return false;
      }

      void udp_read_handler(PacketFrom::SPtr& pfp) // called by LinkImpl
      {
	if (config->server_addr_float || pfp->sender_endpoint == server_endpoint)
	  parent->transport_recv(pfp->buf);
	else
	  config->stats->error(Error::BAD_SRC_ADDR);
      }

      void stop_()
      {
	if (!halt)
	  {
	    halt = true;
	    if (impl)
	      impl->stop();
	    socket.close();
	    resolver.cancel();
	  }
      }

      // called after DNS resolution has succeeded or failed
      void do_resolve_(const openvpn_io::error_code& error,
		       openvpn_io::ip::udp::resolver::results_type results)
      {
	if (!halt)
	  {
	    if (!error)
	      {
		// save resolved endpoint list in remote_list
		config->remote_list->set_endpoint_range(results);
		start_connect_();
	      }
	    else
	      {
		std::ostringstream os;
		os << "DNS resolve error on '" << server_host << "' for UDP session: " << error.message();
		config->stats->error(Error::RESOLVE_ERROR);
		stop();
		parent->transport_error(Error::UNDEF, os.str());
	      }
	  }
      }

      // do UDP connect
      void start_connect_()
      {
	config->remote_list->get_endpoint(server_endpoint);
	OPENVPN_LOG("Contacting " << server_endpoint << " via UDP");
	parent->transport_wait();
	parent->ip_hole_punch(server_endpoint_addr());
	socket.open(server_endpoint.protocol());
#if defined(OPENVPN_PLATFORM_TYPE_UNIX) || defined(OPENVPN_PLATFORM_UWP)
	if (config->socket_protect)
	  {
	    if (!config->socket_protect->socket_protect(socket.native_handle()))
	      {
		config->stats->error(Error::SOCKET_PROTECT_ERROR);
		stop();
		parent->transport_error(Error::UNDEF, "socket_protect error (UDP)");
		return;
	      }
	  }
#endif
	socket.async_connect(server_endpoint, [self=Ptr(this)](const openvpn_io::error_code& error)
                                              {
                                                self->start_impl_(error);
                                              });
      }

      // start I/O on UDP socket
      void start_impl_(const openvpn_io::error_code& error)
      {
	if (!halt)
	  {
	    if (!error)
	      {
		impl.reset(new LinkImpl(this,
					socket,
					(*config->frame)[Frame::READ_LINK_UDP],
					config->stats));
#ifdef OPENVPN_GREMLIN
		impl->gremlin_config(config->gremlin_config);
#endif
		impl->start(config->n_parallel);
		parent->transport_connecting();
	      }
	    else
	      {
		std::ostringstream os;
		os << "UDP connect error on '" << server_host << ':' << server_port << "' (" << server_endpoint << "): " << error.message();
		config->stats->error(Error::UDP_CONNECT_ERROR);
		stop();
		parent->transport_error(Error::UNDEF, os.str());
	      }
	  }
      }

      std::string server_host;
      std::string server_port;

      openvpn_io::io_context& io_context;
      openvpn_io::ip::udp::socket socket;
      ClientConfig::Ptr config;
      TransportClientParent* parent;
      LinkImpl::Ptr impl;
      openvpn_io::ip::udp::resolver resolver;
      UDPTransport::AsioEndpoint server_endpoint;
      bool halt;
    };

    inline TransportClient::Ptr ClientConfig::new_transport_client_obj(openvpn_io::io_context& io_context,
								       TransportClientParent* parent)
    {
      return TransportClient::Ptr(new Client(io_context, this, parent));
    }
  }
} // namespace openvpn

#endif
