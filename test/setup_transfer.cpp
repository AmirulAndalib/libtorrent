/*

Copyright (c) 2008, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include <fstream>
#include <sstream>

#include "libtorrent/session.hpp"
#include "libtorrent/hasher.hpp"
#include "libtorrent/http_parser.hpp"

#include <boost/thread.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/convenience.hpp>
#include <boost/bind.hpp>

#include "test.hpp"
#include "libtorrent/assert.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/create_torrent.hpp"

using boost::filesystem::remove_all;
using boost::filesystem::create_directory;
using namespace libtorrent;
namespace sf = boost::filesystem;

bool tests_failure = false;

void report_failure(char const* err, char const* file, int line)
{
	std::cerr << "\033[31m" << file << ":" << line << " \"" << err << "\"\033[0m\n";
	tests_failure = true;
}

bool print_alerts(libtorrent::session& ses, char const* name
	, bool allow_disconnects, bool allow_no_torrents, bool allow_failed_fastresume
	, bool (*predicate)(libtorrent::alert*))
{
	bool ret = false;
	std::vector<torrent_handle> handles = ses.get_torrents();
	TEST_CHECK(!handles.empty() || allow_no_torrents);
	torrent_handle h;
	if (!handles.empty()) h = handles[0];
	std::auto_ptr<alert> a;
	a = ses.pop_alert();
	while (a.get())
	{
		if (predicate && predicate(a.get())) ret = true;
		if (peer_disconnected_alert* p = dynamic_cast<peer_disconnected_alert*>(a.get()))
		{
			std::cerr << name << "(" << p->ip << "): " << p->message() << "\n";
		}
		else if (a->message() != "block downloading"
			&& a->message() != "block finished"
			&& a->message() != "piece finished")
		{
			std::cerr << name << ": " << a->message() << "\n";
		}
		TEST_CHECK(dynamic_cast<fastresume_rejected_alert*>(a.get()) == 0 || allow_failed_fastresume);

		TEST_CHECK(dynamic_cast<peer_error_alert*>(a.get()) == 0
			|| (!handles.empty() && h.is_seed())
			|| a->message() == "connecting to peer"
			|| a->message() == "closing connection to ourself"
			|| a->message() == "duplicate connection"
			|| a->message() == "duplicate peer-id, connection closed"
			|| (allow_disconnects && a->message() == "Broken pipe")
			|| (allow_disconnects && a->message() == "Connection reset by peer")
			|| (allow_disconnects && a->message() == "End of file."));
		a = ses.pop_alert();
	}
	return ret;
}

void test_sleep(int millisec)
{
	boost::xtime xt;
	boost::xtime_get(&xt, boost::TIME_UTC);
	boost::uint64_t nanosec = (millisec % 1000) * 1000000 + xt.nsec;
	int sec = millisec / 1000;
	if (nanosec > 1000000000)
	{
		nanosec -= 1000000000;
		sec++;
	}
	xt.nsec = nanosec;
	xt.sec += sec;
	boost::thread::sleep(xt);
}

void stop_proxy(int port)
{
	char buf[100];
	snprintf(buf, sizeof(buf), "delegated -P%d -Fkill", port);
	system(buf);
}

void start_proxy(int port, int proxy_type)
{
	using namespace libtorrent;

	stop_proxy(port);

	char const* type = "";
	char const* auth = "";

	switch (proxy_type)
	{
		case proxy_settings::socks4:
			type = "socks4";
			break;
		case proxy_settings::socks5:
			type = "socks5";
			break;
		case proxy_settings::socks5_pw:
			type = "socks5";
			auth = "AUTHORIZER=-list{testuser:testpass}";
			break;
		case proxy_settings::http:
			type = "http";
			break;
		case proxy_settings::http_pw:
			type = "http";
			auth = "AUTHORIZER=-list{testuser:testpass}";
			break;
	}

	char buf[512];
	// we need to echo n since dg will ask us to configure it
	snprintf(buf, sizeof(buf), "echo n | delegated -P%d ADMIN=test@test.com "
		"PERMIT=\"*:*:localhost\" REMITTABLE=+,https RELAY=proxy,delegate "
		"SERVER=%s %s"
		, port, type, auth);

	system(buf);
	test_sleep(1000);
}

using namespace libtorrent;

template <class T>
boost::intrusive_ptr<T> clone_ptr(boost::intrusive_ptr<T> const& ptr)
{
	return boost::intrusive_ptr<T>(new T(*ptr));
}

boost::intrusive_ptr<torrent_info> create_torrent(std::ostream* file, int piece_size, int num_pieces)
{
	char const* tracker_url = "http://non-existent-name.com/announce";
	// excercise the path when encountering invalid urls
	char const* invalid_tracker_url = "http:";
	char const* invalid_tracker_protocol = "foo://non/existent-name.com/announce";
	
	using namespace boost::filesystem;

	file_storage fs;
	int total_size = piece_size * num_pieces;
	fs.add_file(path("temporary"), total_size);
	libtorrent::create_torrent t(fs, piece_size);
	t.add_tracker(tracker_url);
	t.add_tracker(invalid_tracker_url);
	t.add_tracker(invalid_tracker_protocol);

	std::vector<char> piece(piece_size);
	for (int i = 0; i < int(piece.size()); ++i)
		piece[i] = (i % 26) + 'A';
	
	// calculate the hash for all pieces
	int num = t.num_pieces();
	sha1_hash ph = hasher(&piece[0], piece.size()).final();
	for (int i = 0; i < num; ++i)
		t.set_hash(i, ph);

	if (file)
	{
		while (total_size > 0)
		{
			file->write(&piece[0], (std::min)(int(piece.size()), total_size));
			total_size -= piece.size();
		}
	}
	
	std::vector<char> tmp;
	std::back_insert_iterator<std::vector<char> > out(tmp);
	bencode(out, t.generate());
	return boost::intrusive_ptr<torrent_info>(new torrent_info(&tmp[0], tmp.size()));
}

boost::tuple<torrent_handle, torrent_handle, torrent_handle>
setup_transfer(session* ses1, session* ses2, session* ses3
	, bool clear_files, bool use_metadata_transfer, bool connect_peers
	, std::string suffix, int piece_size
	, boost::intrusive_ptr<torrent_info>* torrent, bool super_seeding
	, add_torrent_params const* p)
{
	using namespace boost::filesystem;

	assert(ses1);
	assert(ses2);

	session_settings sess_set;
	sess_set.allow_multiple_connections_per_ip = true;
	sess_set.ignore_limits_on_local_network = false;
	ses1->set_settings(sess_set);
	ses2->set_settings(sess_set);
	if (ses3) ses3->set_settings(sess_set);
	ses1->set_alert_mask(~alert::progress_notification);
	ses2->set_alert_mask(~alert::progress_notification);
	if (ses3) ses3->set_alert_mask(~alert::progress_notification);

	std::srand(time(0));
	peer_id pid;
	std::generate(&pid[0], &pid[0] + 20, std::rand);
	ses1->set_peer_id(pid);
	std::generate(&pid[0], &pid[0] + 20, std::rand);
	ses2->set_peer_id(pid);
	assert(ses1->id() != ses2->id());
	if (ses3)
	{
		std::generate(&pid[0], &pid[0] + 20, std::rand);
		ses3->set_peer_id(pid);
		assert(ses3->id() != ses2->id());
	}

	boost::intrusive_ptr<torrent_info> t;
	if (torrent == 0)
	{
		create_directory("./tmp1" + suffix);
		std::ofstream file(("./tmp1" + suffix + "/temporary").c_str());
		t = ::create_torrent(&file, piece_size, 19);
		file.close();
		if (clear_files)
		{
			remove_all("./tmp2" + suffix + "/temporary");
			remove_all("./tmp3" + suffix + "/temporary");
		}
		char ih_hex[41];
		to_hex((char const*)&t->info_hash()[0], 20, ih_hex);
		std::cerr << "generated torrent: " << ih_hex << " ./tmp1" << suffix << "/temporary" << std::endl;
	}
	else
	{
		t = *torrent;
	}

	// they should not use the same save dir, because the
	// file pool will complain if two torrents are trying to
	// use the same files
	sha1_hash info_hash = t->info_hash();
	add_torrent_params param;
	if (p) param = *p;
	param.ti = clone_ptr(t);
	param.save_path = "./tmp1" + suffix;
	torrent_handle tor1 = ses1->add_torrent(param);
	tor1.super_seeding(super_seeding);
	TEST_CHECK(!ses1->get_torrents().empty());
	torrent_handle tor2;
	torrent_handle tor3;

	// the downloader cannot use seed_mode
	param.seed_mode = false;

	if (ses3)
	{
		param.ti = clone_ptr(t);
		param.save_path = "./tmp3" + suffix;
		tor3 = ses3->add_torrent(param);
		TEST_CHECK(!ses3->get_torrents().empty());
	}

  	if (use_metadata_transfer)
	{
		param.ti = 0;
		param.info_hash = t->info_hash();
	}
	else
	{
		param.ti = clone_ptr(t);
	}
	param.save_path = "./tmp2" + suffix;

	tor2 = ses2->add_torrent(param);
	TEST_CHECK(!ses2->get_torrents().empty());

	assert(ses1->get_torrents().size() == 1);
	assert(ses2->get_torrents().size() == 1);

	test_sleep(100);

	if (connect_peers)
	{
		std::cerr << "connecting peer\n";
		error_code ec;
		tor1.connect_peer(tcp::endpoint(address::from_string("127.0.0.1", ec)
			, ses2->listen_port()));

		if (ses3)
		{
			// give the other peers some time to get an initial
			// set of pieces before they start sharing with each-other
			tor3.connect_peer(tcp::endpoint(
				address::from_string("127.0.0.1", ec)
				, ses2->listen_port()));
			tor3.connect_peer(tcp::endpoint(
				address::from_string("127.0.0.1", ec)
				, ses1->listen_port()));
		}
	}

	return boost::make_tuple(tor1, tor2, tor3);
}


socket_acceptor* web_socket = 0;
boost::shared_ptr<boost::thread> web_server;

void stop_web_server(int port)
{
	if (web_server && web_socket)
	{
		web_socket->close();
		web_server->join();
		web_server.reset();
		web_socket = 0;
	}
}

void web_server_thread(int port, bool ssl);

void start_web_server(int port, bool ssl)
{
	stop_web_server(port);

	web_server.reset(new boost::thread(boost::bind(&web_server_thread, port, ssl)));

	// create this directory so that the path
	// "relative/../test_file" can resolve
	create_directory("relative");
	test_sleep(100);
}

void send_response(stream_socket& s, error_code& ec
	, int code, char const* status_message, char const* extra_header
	, int len)
{
	char msg[400];
	int pkt_len = snprintf(msg, sizeof(msg), "HTTP/1.0 %d %s\r\n"
		"content-length: %d\r\n"
		"connection: close\r\n"
		"%s%s"
		"\r\n"
		, code, status_message, len
		, extra_header ? extra_header : "", extra_header ? "\r\n" : "");
	fprintf(stderr, ">> %s\n", msg);
	write(s, boost::asio::buffer(msg, pkt_len), boost::asio::transfer_all(), ec);
}

void web_server_thread(int port, bool ssl)
{
	// TODO: support SSL

	io_service ios;
	socket_acceptor acceptor(ios);
	web_socket = &acceptor;
	error_code ec;
	acceptor.open(tcp::v4(), ec);
	if (ec)
	{
		fprintf(stderr, "Error opening listen socket: %s\n", ec.message().c_str());
		return;
	}
	acceptor.set_option(socket_acceptor::reuse_address(true), ec);
	if (ec)
	{
		fprintf(stderr, "Error setting listen socket to reuse addr: %s\n", ec.message().c_str());
		return;
	}
	acceptor.bind(tcp::endpoint(address_v4::any(), port), ec);
	if (ec)
	{
		fprintf(stderr, "Error binding listen socket: %s\n", ec.message().c_str());
		return;
	}
	acceptor.listen(10, ec);
	if (ec)
	{
		fprintf(stderr, "Error listening on socket: %s\n", ec.message().c_str());
		return;
	}

	char buf[10000];
	int len = 0;
	stream_socket s(ios);
	
	for (;;)
	{
		s.close(ec);

		len = 0;
		acceptor.accept(s, ec);
		if (ec)
		{
			fprintf(stderr, "accept failed: %s\n", ec.message().c_str());
			return;
		}

		http_parser p;
		bool failed = false;

		while (!p.finished())
		{
			size_t received = s.read_some(boost::asio::buffer(&buf[len], sizeof(buf) - len), ec);
			if (ec || received <= 0)
			{
				fprintf(stderr, "read failed: %s\n", ec.message().c_str());
				failed = true;
				break;
			}
			len += received;

			bool error = false;
			p.incoming(buffer::const_interval(buf, buf + len), error);
		}

		if (failed) continue;

		if (p.method() != "get" && p.method() != "post")
		{
				fprintf(stderr, "incorrect method: %s\n", p.method().c_str());
				continue;
		}

		std::string path = p.path();

		if (path == "/redirect")
		{
			send_response(s, ec, 301, "Moved Permanently", "Location: /test_file", 0);
			continue;
		}

		if (path == "/infinite_redirect")
		{
			send_response(s, ec, 301, "Moved Permanently", "Location: /infinite_redirect", 0);
			continue;
		}

		if (path == "/relative/redirect")
		{
			send_response(s, ec, 301, "Moved Permanently", "Location: ../test_file", 0);
			continue;
		}

		fprintf(stderr, ">> serving file %s\n", path.c_str());
		std::vector<char> file_buf;
		// remove the / from the path
		path = path.substr(1);
		int res = load_file(path, file_buf);
		if (res == -1)
		{
			send_response(s, ec, 404, "Not Found", 0, 0);
			continue;
		}

		if (res != 0)
		{
			// this means the file was either too big or couldn't be read
			send_response(s, ec, 503, "Internal Error", 0, 0);
			continue;
		}
		
		// serve file

		char const* extra_header = 0;

		if (boost::filesystem::extension(path) == ".gz")
		{
			extra_header = "Content-Encoding: gzip";
		}

		if (!p.header("range").empty())
		{
			std::string range = p.header("range");
			int start, end;
			sscanf(range.c_str(), "bytes=%d-%d", &start, &end);
			send_response(s, ec, 206, "Partial", extra_header, end - start + 1);
			write(s, boost::asio::buffer(&file_buf[0] + start, end - start + 1), boost::asio::transfer_all(), ec);
		}
		else
		{
			send_response(s, ec, 200, "OK", extra_header, file_buf.size());
			write(s, boost::asio::buffer(&file_buf[0], file_buf.size()), boost::asio::transfer_all(), ec);
		}
	}
}


