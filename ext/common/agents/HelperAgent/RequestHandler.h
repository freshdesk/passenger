/*
 *  Phusion Passenger - http://www.modrails.com/
 *  Copyright (c) 2011, 2012 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */

/*
   STAGES
   
     Accept connect password
              |
             \|/
          Read header
              |
             \|/
       +------+------+
       |             |
       |             |
      \|/            |
     Buffer          |
     request         |
     body            |
       |             |
       |             |
      \|/            |
    Checkout <-------+
    session
       |
       |
      \|/
  Send header
    to app
       |
       |
      \|/
  Send request
   body to app



     OVERVIEW OF I/O CHANNELS, PIPES AND WATCHERS


                             OPTIONAL:                                       appOutputWatcher
                          clientBodyBuffer                                         (o)
                                 |                                                  |
    +----------+                 |             +-----------+                        |   +---------------+
    |          |     ------ clientInput -----> |  Request  | ---------------->          |               |
    |  Client  | fd                            |  Handler  |                    session |  Application  |
    |          |     <--- clientOutputPipe --- |           | <--- appInput ---          |               |
    +----------+ |                             +-----------+                            +---------------+
                 |
                (o)
        clientOutputWatcher

 */

#ifndef _PASSENGER_REQUEST_HANDLER_H_
#define _PASSENGER_REQUEST_HANDLER_H_

#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>
#include <boost/make_shared.hpp>
#include <ev++.h>

#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <cassert>
#include <cctype>

#include <Logging.h>
#include <EventedBufferedInput.h>
#include <MessageReadersWriters.h>
#include <Constants.h>
#include <HttpConstants.h>
#include <ApplicationPool2/Pool.h>
#include <Utils/StrIntUtils.h>
#include <Utils/IOUtils.h>
#include <Utils/HttpHeaderBufferer.h>
#include <Utils/Template.h>
#include <agents/HelperAgent/AgentOptions.h>
#include <agents/HelperAgent/FileBackedPipe.h>
#include <agents/HelperAgent/ScgiRequestParser.h>

namespace Passenger {

using namespace std;
using namespace boost;
using namespace oxt;
using namespace ApplicationPool2;

class RequestHandler;

#define RH_WARN(client, x) P_WARN("[Client " << client->name() << "] " << x)
#define RH_DEBUG(client, x) P_DEBUG("[Client " << client->name() << "] " << x)
#define RH_TRACE(client, level, x) P_TRACE(level, "[Client " << client->name() << "] " << x)


class Client: public enable_shared_from_this<Client> {
private:
	struct ev_loop *getLoop() const;
	const SafeLibevPtr &getSafeLibev() const;
	unsigned int getConnectPasswordTimeout(const RequestHandler *handler) const;

	static size_t onClientInputData(const EventedBufferedInputPtr &source, const StaticString &data);
	static void onClientInputError(const EventedBufferedInputPtr &source, const char *message, int errnoCode);

	static void onClientBodyBufferData(const FileBackedPipePtr &source,
		const char *data, size_t size,
		const FileBackedPipe::ConsumeCallback &callback);
	static void onClientBodyBufferEnd(const FileBackedPipePtr &source);
	static void onClientBodyBufferError(const FileBackedPipePtr &source, int errorCode);
	static void onClientBodyBufferCommit(const FileBackedPipePtr &source);
	
	static void onClientOutputPipeData(const FileBackedPipePtr &source,
		const char *data, size_t size,
		const FileBackedPipe::ConsumeCallback &callback);
	static void onClientOutputPipeEnd(const FileBackedPipePtr &source);
	static void onClientOutputPipeError(const FileBackedPipePtr &source, int errorCode);
	static void onClientOutputPipeCommit(const FileBackedPipePtr &source);
	
	void onClientOutputWritable(ev::io &io, int revents);
	
	static size_t onAppInputData(const EventedBufferedInputPtr &source, const StaticString &data);
	static void onAppInputError(const EventedBufferedInputPtr &source, const char *message, int errnoCode);
	
	void onAppOutputWritable(ev::io &io, int revents);

	void onTimeout(ev::timer &timer, int revents);


	void resetPrimitiveFields() {
		requestHandler = NULL;
		state = DISCONNECTED;
		backgroundOperations = 0;
		requestBodyIsBuffered = false;
		freeBufferedConnectPassword();
		sessionCheckedOut = false;
		sessionCheckoutTry = 0;
		responseHeaderSeen = false;
	}

public:
	/** Back reference to the RequestHandler that this Client is associated with.
	 * NULL when this Client is not in the pool or is disconnected. */
	RequestHandler *requestHandler;
	/** File descriptor of the client socket. Is empty when this Client is not
	 * in the pool or is disconnected. */
	FileDescriptor fd;
	/** The last associated file descriptor number is stored here. It is not
	 * cleared after disassociating. Its only purpose is to make logging calls
	 * like RH_DEBUG() print the correct client name after disconnect() is called.
	 * Do not use this value for anything else as it may not refer to a valid
	 * file descriptor. */
	int fdnum;


	/***** Client <-> RequestHandler I/O channels, pipes and watchers *****/

	/** Client input channel. */
	EventedBufferedInputPtr clientInput;
	/** If request body buffering is turned on, it will be buffered into this FileBackedPipe. */
	FileBackedPipePtr clientBodyBuffer;
	/** Client output pipe. */
	FileBackedPipePtr clientOutputPipe;
	/** Client output channel watcher. */
	ev::io clientOutputWatcher;


	/***** RequestHandler <-> Application I/O channels, pipes and watchers *****/

	/** Application input channel. */
	EventedBufferedInputPtr appInput;
	string appOutputBuffer;
	/** Application output channel watcher. */
	ev::io appOutputWatcher;


	/***** State variables *****/

	enum {
		BEGIN_READING_CONNECT_PASSWORD,
		STILL_READING_CONNECT_PASSWORD,
		READING_HEADER,
		BUFFERING_REQUEST_BODY,
		CHECKING_OUT_SESSION,
		SENDING_HEADER_TO_APP,
		FORWARDING_BODY_TO_APP,

		// Special states
		WRITING_SIMPLE_RESPONSE,
		DISCONNECTED
	} state;

	/* How many background operations are currently in progress, e.g.
	 * an asyncGet() or bodyBuffer.add(). If the client is disconnected
	 * while this flag is true, then the Client object is not reassociateable
	 * in order to give the completion callbacks a chance to cancel properly.
	 */
	unsigned int backgroundOperations;

	struct {
		char *data;
		unsigned int alreadyRead;
	} bufferedConnectPassword;

	// Used for enforcing the connection timeout.
	ev::timer timeoutTimer;

	Options options;
	ScgiRequestParser scgiParser;
	SessionPtr session;
	unsigned int sessionCheckoutTry;
	bool requestBodyIsBuffered;
	bool sessionCheckedOut;

	bool responseHeaderSeen;
	HttpHeaderBufferer responseHeaderBufferer;


	Client() {
		fdnum = -1;

		clientInput = make_shared< EventedBufferedInput<> >();
		clientInput->onData   = onClientInputData;
		clientInput->onError  = onClientInputError;
		clientInput->userData = this;
		
		clientBodyBuffer = make_shared<FileBackedPipe>("/tmp");
		clientBodyBuffer->userData  = this;
		clientBodyBuffer->onData    = onClientBodyBufferData;
		clientBodyBuffer->onEnd     = onClientBodyBufferEnd;
		clientBodyBuffer->onError   = onClientBodyBufferError;
		clientBodyBuffer->onCommit  = onClientBodyBufferCommit;

		clientOutputPipe = make_shared<FileBackedPipe>("/tmp");
		clientOutputPipe->userData  = this;
		clientOutputPipe->onData    = onClientOutputPipeData;
		clientOutputPipe->onEnd     = onClientOutputPipeEnd;
		clientOutputPipe->onError   = onClientOutputPipeError;
		clientOutputPipe->onCommit  = onClientOutputPipeCommit;

		clientOutputWatcher.set<Client, &Client::onClientOutputWritable>(this);

		
		appInput = make_shared< EventedBufferedInput<> >();
		appInput->onData   = onAppInputData;
		appInput->onError  = onAppInputError;
		appInput->userData = this;
		
		appOutputWatcher.set<Client, &Client::onAppOutputWritable>(this);


		timeoutTimer.set<Client, &Client::onTimeout>(this);
		

		bufferedConnectPassword.data = NULL;
		bufferedConnectPassword.alreadyRead = 0;
		resetPrimitiveFields();
	}

	~Client() {
		clientInput->userData      = NULL;
		clientBodyBuffer->userData = NULL;
		clientOutputPipe->userData = NULL;
		appInput->userData         = NULL;
		freeBufferedConnectPassword();
	}

	void associate(RequestHandler *handler, const FileDescriptor &_fd) {
		assert(requestHandler == NULL);
		requestHandler = handler;
		fd = _fd;
		fdnum = _fd;
		state = BEGIN_READING_CONNECT_PASSWORD;

		clientInput->reset(getSafeLibev().get(), _fd);
		clientInput->start();
		clientBodyBuffer->reset(getSafeLibev());
		clientOutputPipe->reset(getSafeLibev());
		clientOutputPipe->start();
		clientOutputWatcher.set(getLoop());

		appOutputWatcher.set(getLoop());

		timeoutTimer.set(getLoop());
		timeoutTimer.start(getConnectPasswordTimeout(handler) / 1000.0, 0.0);
	}

	void disassociate() {
		assert(requestHandler != NULL);
		resetPrimitiveFields();
		fd = FileDescriptor();

		clientInput->reset(NULL, FileDescriptor());
		clientBodyBuffer->reset();
		clientOutputPipe->reset();
		clientOutputWatcher.stop();

		appInput->reset(NULL, FileDescriptor());
		appOutputBuffer.resize(0);
		appOutputWatcher.stop();
		
		timeoutTimer.stop();
		scgiParser.reset();
		session.reset();
		responseHeaderBufferer.reset();
	}

	void discard() {
		assert(requestHandler != NULL);
		resetPrimitiveFields();
		fd = FileDescriptor();

		clientInput->stop();
		clientBodyBuffer->stop();
		clientOutputPipe->stop();
		clientOutputWatcher.stop();

		appInput->stop();
		appOutputWatcher.stop();

		timeoutTimer.stop();
	}

	bool reassociateable() const {
		return requestHandler == NULL
			&& backgroundOperations == 0
			&& clientInput->resetable()
			&& clientBodyBuffer->resetable()
			&& clientOutputPipe->resetable()
			&& appInput->resetable();
	}

	string name() const {
		if (fdnum == -1) {
			return "(null)";
		} else {
			return toString(fdnum);
		}
	}

	bool connected() const {
		return requestHandler != NULL;
	}

	const char *getStateName() const {
		switch (state) {
		case BEGIN_READING_CONNECT_PASSWORD:
			return "BEGIN_READING_CONNECT_PASSWORD";
		case STILL_READING_CONNECT_PASSWORD:
			return "STILL_READING_CONNECT_PASSWORD";
		case READING_HEADER:
			return "READING_HEADER";
		case BUFFERING_REQUEST_BODY:
			return "BUFFERING_REQUEST_BODY";
		case CHECKING_OUT_SESSION:
			return "CHECKING_OUT_SESSION";
		case SENDING_HEADER_TO_APP:
			return "SENDING_HEADER_TO_APP";
		case FORWARDING_BODY_TO_APP:
			return "FORWARDING_BODY_TO_APP";
		case WRITING_SIMPLE_RESPONSE:
			return "WRITING_SIMPLE_RESPONSE";
		case DISCONNECTED:
			return "DISCONNECTED";
		default:
			return "UNKNOWN";
		}
	}

	void freeBufferedConnectPassword() {
		if (bufferedConnectPassword.data != NULL) {
			free(bufferedConnectPassword.data);
			bufferedConnectPassword.data = NULL;
			bufferedConnectPassword.alreadyRead = 0;
		}
	}

	void verifyInvariants() const {
		assert((requestHandler == NULL) == (fd == -1));
		assert((requestHandler == NULL) == (state == DISCONNECTED));
	}

	template<typename Stream>
	void inspect(Stream &stream) const {
		const char *indent = "    ";
		stream
			<< indent << "state = " << getStateName() << "\n"
			<< indent << "requestBodyIsBuffered    = " << requestBodyIsBuffered << "\n"
			<< indent << "responseHeaderSeen       = " << responseHeaderSeen << "\n"
			<< indent << "clientInput started      = " << clientInput->isStarted() << "\n"
			<< indent << "clientOutputPipe started = " << clientOutputPipe->isStarted() << "\n"
			;
	}
};

typedef shared_ptr<Client> ClientPtr;


class RequestHandler {
private:
	friend class Client;

	const SafeLibevPtr libev;
	FileDescriptor requestSocket;
	PoolPtr pool;
	const AgentOptions &options;
	const ResourceLocator resourceLocator;
	ev::io requestSocketWatcher;
	HashMap<int, ClientPtr> clients;
	bool accept4Available;


	void disconnect(const ClientPtr &client) {
		// Prevent Client object from being destroyed until we're done.
		ClientPtr reference = client;

		clients.erase(client->fd);
		client->discard();
		client->verifyInvariants();
		RH_DEBUG(client, "Disconnected; new client count = " << clients.size());
	}

	void disconnectWithError(const ClientPtr &client, const StaticString &message) {
		RH_WARN(client, "Disconnecting with error: " << message);
		disconnect(client);
	}

	void disconnectWithClientSocketWriteError(const ClientPtr &client, int e) {
		stringstream message;
		message << "client socket write error: ";
		message << strerror(e);
		message << " (errno " << e << ")";
		disconnectWithError(client, message.str());
	}

	void disconnectWithAppSocketWriteError(const ClientPtr &client, int e) {
		stringstream message;
		message << "app socket write error: ";
		message << strerror(e);
		message << " (errno " << e << ")";
		disconnectWithError(client, message.str());
	}

	void disconnectWithWarning(const ClientPtr &client, const StaticString &message) {
		P_DEBUG("Disconnected client " << client->name() << " with warning: " << message);
		disconnect(client);
	}

	// GDB helper function, implemented in .cpp file to prevent inlining.
	Client *getClientPointer(const ClientPtr &client);

	static bool getBoolOption(const ClientPtr &client, const StaticString &name, bool defaultValue = false) {
		ScgiRequestParser::const_iterator it = client->scgiParser.getHeaderIterator(name);
		if (it != client->scgiParser.end()) {
			return it->second == "true";
		} else {
			return defaultValue;
		}
	}

	void writeErrorResponse(const ClientPtr &client, const StaticString &message, const SpawnException *e = NULL) {
		assert(client->state < Client::FORWARDING_BODY_TO_APP);
		client->state = Client::WRITING_SIMPLE_RESPONSE;

		string templatesDir = resourceLocator.getResourcesDir() + "/templates";
		string data;

		if (getBoolOption(client, "PASSENGER_FRIENDLY_ERROR_PAGES", true)) {
			string cssFile = templatesDir + "/error_layout.css";
			string errorLayoutFile = templatesDir + "/error_layout.html.template";
			string generalErrorFile =
				(e != NULL && e->isHTML())
				? templatesDir + "/general_error_with_html.html.template"
				: templatesDir + "/general_error.html.template";
			string css = readAll(cssFile);
			StringMap<StaticString> params;

			params.set("CSS", css);
			params.set("APP_ROOT", client->options.appRoot);
			params.set("ENVIRONMENT", client->options.environment);
			params.set("MESSAGE", message);
			if (e != NULL) {
				params.set("TITLE", "Web application could not be started");
				// Store all SpawnException annotations into 'params',
				// but convert its name to uppercase.
				const map<string, string> &annotations = e->getAnnotations();
				map<string, string>::const_iterator it, end = annotations.end();
				for (it = annotations.begin(); it != end; it++) {
					string name = it->first;
					for (string::size_type i = 0; i < name.size(); i++) {
						name[i] = toupper(name[i]);
					}
					params.set(name, it->second);
				}
			} else {
				params.set("TITLE", "Internal server error");
			}
			string content = Template::apply(readAll(generalErrorFile), params);
			params.set("CONTENT", content);
			data = Template::apply(readAll(errorLayoutFile), params);
		} else {
			data = readAll(templatesDir + "/undisclosed_error.html.template");
		}

		stringstream str;
		if (getBoolOption(client, "PASSENGER_PRINT_STATUS_LINE", true)) {
			str << "HTTP/1.1 500 Internal Server Error\r\n";
		}
		str << "Status: 500 Internal Server Error\r\n";
		str << "Content-Length: " << data.size() << "\r\n";
		str << "Content-Type: text/html; charset=UTF-8\r\n";
		str << "\r\n";

		const string &header = str.str();
		client->clientOutputPipe->write(header.data(), header.size());
		client->clientOutputPipe->write(data.data(), data.size());
		client->clientOutputPipe->end();
	}


	/*****************************************************
	 * COMPONENT: appInput -> clientOutputPipe plumbing
	 *
	 * The following code receives data from appInput,
	 * possibly modifies it, and forwards it to
	 * clientOutputPipe.
	 *****************************************************/
	
	struct Header {
		StaticString key;
		StaticString value;

		Header() { }

		Header(const StaticString &_key, const StaticString &_value)
			: key(_key),
			  value(_value)
			{ }
		
		bool empty() const {
			return key.empty();
		}

		const char *begin() const {
			return key.data();
		}

		const char *end() const {
			return value.data() + value.size() + sizeof("\r\n") - 1;
		}

		size_t size() const {
			return end() - begin();
		}
	};

	static char *appendData(char *pos, const char *end, const StaticString &data) {
		size_t size = std::min<size_t>(end - pos, data.size());
		memcpy(pos, data.data(), size);
		return pos + size;
	}

	/** Given a substring containing the start of the header value,
	 * extracts the substring that contains a single header value.
	 *
	 *   const char *data =
	 *      "Status: 200 OK\r\n"
	 *      "Foo: bar\r\n";
	 *   extractHeaderValue(data + strlen("Status:"), strlen(data) - strlen("Status:"));
	 *      // "200 OK"
	 */
	static StaticString extractHeaderValue(const char *data, size_t size) {
		const char *start = data;
		const char *end   = data + size;
		const char *terminator;
		
		while (start < end && *start == ' ') {
			start++;
		}
		
		terminator = (const char *) memchr(start, '\r', end - start);
		if (terminator == NULL) {
			return StaticString();
		} else {
			return StaticString(start, terminator - start);
		}
	}

	static Header lookupHeader(const StaticString &headerData, const StaticString &name) {
		string::size_type searchStart = 0;
		while (searchStart < headerData.size()) {
			string::size_type pos = headerData.find(name, searchStart);
			if (OXT_UNLIKELY(pos == string::npos)) {
				return Header();
			} else if ((pos == 0 || headerData[pos - 1] == '\n')
				&& headerData.size() > pos + name.size()
				&& headerData[pos + name.size()] == ':')
			{
				StaticString value = extractHeaderValue(
					headerData.data() + pos + name.size() + 1,
					headerData.size() - pos - name.size() - 1);
				return Header(headerData.substr(pos, name.size()), value);
			} else {
				searchStart = pos + name.size() + 1;
			}
		}
		return Header();
	}

	/*
	 * Given a full header and possibly some rest data, possibly modify the header
	 * and send both to the clientOutputPipe.
	 */
	void processResponseHeader(const ClientPtr &client, const string &currentPacket,
		const StaticString &headerData, const StaticString &rest)
	{
		/* Note: we don't strip out the Status header because some broken HTTP clients depend on it.
		 * http://groups.google.com/group/phusion-passenger/browse_thread/thread/03e0381684fbae09
		 */
		
		/* 'newHeader' contains the modified header. If empty, it means
		 * the header has not been modified, in which case 'headerData' should be used.
		 * 'prefix' contains data that we want to send before the header
		 * (before both 'headerData' and 'newHeader'.
		 */
		string prefix, newHeaderData;

		Header status = lookupHeader(headerData, "Status");
		if (status.empty()) {
			disconnectWithError(client, "application sent malformed response: it didn't send a Status header.");
			return;
		}

		if (status.value.find(' ') == string::npos) {
			// Status header contains no reason phrase; add it.

			int statusCode = stringToInt(status.value);
			const char *statusCodeAndReasonPhrase = getStatusCodeAndReasonPhrase(statusCode);
			char newStatus[100];
			char *pos = newStatus;
			const char *end = newStatus + 100;

			pos = appendData(pos, end, "Status: ");
			if (statusCodeAndReasonPhrase == NULL) {
				pos = appendData(pos, end, toString(statusCode));
				pos = appendData(pos, end, " Unknown Reason-Phrase\r\n");
			} else {
				pos = appendData(pos, end, statusCodeAndReasonPhrase);
				pos = appendData(pos, end, "\r\n");
			}

			newHeaderData = headerData;
			newHeaderData.replace(status.begin() - headerData.data(), status.size(),
				newStatus, pos - newStatus);
			status = Header();
		}

		if (getBoolOption(client, "PASSENGER_PRINT_STATUS_LINE", true)) {
			// Prepend HTTP status line.

			if (status.empty()) {
				assert(!newHeaderData.empty());
				status = lookupHeader(newHeaderData, "Status");
			}
			prefix.reserve(prefix.size() + status.value.size() + sizeof("HTTP/1.1\r\n") - 1);
			prefix.append("HTTP/1.1 ");
			prefix.append(status.value);
			prefix.append("\r\n");
		}

		// Add X-Powered-By.
		prefix.append("X-Powered-By: Phusion Passenger " PASSENGER_VERSION "\r\n");


		if (prefix.empty() && newHeaderData.empty()) {
			// Header has not been modified.
			if (headerData.data() == currentPacket.data()) {
				/* The header occurs in the first packet that the
				 * application sent, so send the entire packet.
				 */
				writeToClientOutputPipe(client, currentPacket);
			} else {
				/* It didn't occur in the first packet that the application
				 * sent, so send out the full header and whatever rest data
				 * that we've already received.
				 */
				writeToClientOutputPipe(client, headerData);
				writeToClientOutputPipe(client, rest);
			}
		} else {
			// Header has been modified.
			if (newHeaderData.empty()) {
				prefix.reserve(headerData.size() + rest.size());
				prefix.append(headerData);
			} else {
				prefix.reserve(newHeaderData.size() + rest.size());
				prefix.append(newHeaderData);
			}
			prefix.append(rest);
			writeToClientOutputPipe(client, prefix);
		}
	}

	void writeToClientOutputPipe(const ClientPtr &client, const StaticString &data) {
		bool wasCommittingToDisk = client->clientOutputPipe->isCommittingToDisk();
		bool nowCommittingToDisk = !client->clientOutputPipe->write(data.data(), data.size());
		if (!wasCommittingToDisk && nowCommittingToDisk) {
			client->backgroundOperations++;
			client->appInput->stop();
		}
	}

	size_t onAppInputData(const ClientPtr &client, const StaticString &data) {
		if (!client->connected()) {
			return 0;
		}

		if (!data.empty()) {
			RH_TRACE(client, 3, "Application sent data: \"" << cEscapeString(data) << "\"");

			// Buffer the application response until we've encountered the end of the header.
			if (!client->responseHeaderSeen) {
				size_t consumed = client->responseHeaderBufferer.feed(data.data(), data.size());
				if (!client->responseHeaderBufferer.acceptingInput()) {
					if (client->responseHeaderBufferer.hasError()) {
						disconnectWithError(client, "application response format error (invalid header)");
					} else {
						// Now that we have a full header, do something with it.
						client->responseHeaderSeen = true;
						StaticString header = client->responseHeaderBufferer.getData();
						StaticString rest = data.substr(consumed);
						processResponseHeader(client, data, header, rest);
					}
				}
			} else {
				// The header has already been processed so forward it
				// directly to clientOutputPipe.
				writeToClientOutputPipe(client, data);
			}
			return data.size();

		} else {
			onAppInputEof(client);
			return 0;
		}
	}

	void onAppInputEof(const ClientPtr &client) {
		if (!client->connected()) {
			return;
		}

		RH_TRACE(client, 3, "Application sent EOF");
		client->clientOutputPipe->end();
		client->appInput->stop();
	}

	void onAppInputError(const ClientPtr &client, const char *message, int errorCode) {
		if (!client->connected()) {
			return;
		}

		if (errorCode == ECONNRESET) {
			// We might as well treat ECONNRESET like an EOF.
			// http://stackoverflow.com/questions/2974021/what-does-econnreset-mean-in-the-context-of-an-af-local-socket
			onAppInputEof(client);
		} else {
			stringstream message;
			message << "application socket read error: ";
			message << strerror(errorCode);
			message << " (errno " << errorCode << ")";
			disconnectWithError(client, message.str());
		}
	}

	void onClientOutputPipeCommit(const ClientPtr &client) {
		if (!client->connected()) {
			return;
		}

		client->backgroundOperations--;
		client->appInput->start();
	}


	/*****************************************************
	 * COMPONENT: clientOutputPipe -> client fd plumbing
	 *
	 * The following code handles forwarding data from
	 * clientOutputPipe to the client socket.
	 *****************************************************/

	void onClientOutputPipeData(const ClientPtr &client, const char *data,
		size_t size, const FileBackedPipe::ConsumeCallback &consumed)
	{
		if (!client->connected()) {
			return;
		}

		ssize_t ret = syscalls::write(client->fd, data, size);
		if (ret == -1) {
			if (errno == EAGAIN) {
				// Wait until the client socket is writable before resuming writing data.
				client->clientOutputWatcher.start();
			} else if (errno == EPIPE) {
				// If the client closed the connection then disconnect quietly.
				disconnect(client);
			} else {
				disconnectWithClientSocketWriteError(client, errno);
			}
			consumed(0, true);
		} else {
			consumed(ret, false);
		}
	}

	void onClientOutputPipeEnd(const ClientPtr &client) {
		if (!client->connected()) {
			return;
		}

		RH_TRACE(client, 2, "Client output pipe ended; disconnecting client");
		disconnect(client);
	}

	void onClientOutputPipeError(const ClientPtr &client, int errorCode) {
		if (!client->connected()) {
			return;
		}

		stringstream message;
		message << "client output pipe error: ";
		message << strerror(errorCode);
		message << " (errno " << errorCode << ")";
		disconnectWithError(client, message.str());
	}

	void onClientOutputWritable(const ClientPtr &client) {
		if (!client->connected()) {
			return;
		}

		// Continue forwarding output data to the client.
		client->clientOutputWatcher.stop();
		assert(!client->clientOutputPipe->isStarted());
		client->clientOutputPipe->start();
	}


	/*****************************************************
	 * COMPONENT: client acceptor
	 *
	 * The following code accepts new client connections
	 * and forwards events to the appropriate functions
	 * depending on the client state.
	 *****************************************************/

	FileDescriptor acceptNonBlockingSocket(int sock) {
		union {
			struct sockaddr_in inaddr;
			struct sockaddr_un unaddr;
		} u;
		socklen_t addrlen = sizeof(u);

		if (accept4Available) {
			FileDescriptor fd = callAccept4(requestSocket,
				(struct sockaddr *) &u, &addrlen, O_NONBLOCK);
			if (fd == -1 && errno == ENOSYS) {
				accept4Available = false;
				return acceptNonBlockingSocket(sock);
			} else {
				return fd;
			}
		} else {
			FileDescriptor fd = syscalls::accept(requestSocket,
				(struct sockaddr *) &u, &addrlen);
			if (fd != -1) {
				int e = errno;
				setNonBlocking(fd);
				errno = e;
			}
			return fd;
		}
	}


	void onAcceptable(ev::io &io, int revents) {
		bool endReached = false;
		unsigned int count = 0;

		while (!endReached && count < 10) {
			FileDescriptor fd = acceptNonBlockingSocket(requestSocket);
			if (fd == -1) {
				if (errno == EAGAIN) {
					endReached = true;
				} else {
					int e = errno;
					throw SystemException("Cannot accept client", e);
				}
			} else {
				ClientPtr client = make_shared<Client>();
				client->associate(this, fd);
				clients.insert(make_pair<int, ClientPtr>(fd, client));
				count++;
				RH_DEBUG(client, "New client accepted; new client count = " << clients.size());
			}
		}
	}


	size_t onClientInputData(const ClientPtr &client, const StaticString &data) {
		if (!client->connected()) {
			return 0;
		}

		if (data.empty()) {
			onClientEof(client);
			return 0;
		} else {
			return onClientRealData(client, data.data(), data.size());
		}
	}

	size_t onClientRealData(const ClientPtr &client, const char *buf, size_t size) {
		size_t consumed = 0;

		while (consumed < size && client->connected() && client->clientInput->isStarted()) {
			const char *data = buf + consumed;
			size_t len       = size - consumed;
			size_t locallyConsumed;

			RH_TRACE(client, 3, "Processing client data: \"" << cEscapeString(StaticString(data, len)) << "\"");
			switch (client->state) {
			case Client::BEGIN_READING_CONNECT_PASSWORD:
				locallyConsumed = state_beginReadingConnectPassword_onClientData(client, data, len);
				break;
			case Client::STILL_READING_CONNECT_PASSWORD:
				locallyConsumed = state_stillReadingConnectPassword_onClientData(client, data, len);
				break;
			case Client::READING_HEADER:
				locallyConsumed = state_readingHeader_onClientData(client, data, len);
				break;
			case Client::BUFFERING_REQUEST_BODY:
				locallyConsumed = state_bufferingRequestBody_onClientData(client, data, len);
				break;
			case Client::FORWARDING_BODY_TO_APP:
				locallyConsumed = state_forwardingBodyToApp_onClientData(client, data, len);
				break;
			default:
				abort();
			}

			consumed += locallyConsumed;
			RH_TRACE(client, 3, "Processed client data: consumed " << locallyConsumed << " bytes");
			assert(consumed <= size);
		}

		return consumed;
	}

	void onClientEof(const ClientPtr &client) {
		RH_TRACE(client, 3, "Client sent EOF");
		switch (client->state) {
		case Client::BUFFERING_REQUEST_BODY:
			state_bufferingRequestBody_onClientEof(client);
			break;
		case Client::FORWARDING_BODY_TO_APP:
			state_forwardingBodyToApp_onClientEof(client);
			break;
		default:
			disconnect(client);
			break;
		}
	}

	void onClientInputError(const ClientPtr &client, const char *message, int errnoCode) {
		if (!client->connected()) {
			return;
		}

		if (errnoCode == ECONNRESET) {
			// We might as well treat ECONNRESET like an EOF.
			// http://stackoverflow.com/questions/2974021/what-does-econnreset-mean-in-the-context-of-an-af-local-socket
			onClientEof(client);
		} else {
			stringstream message;
			message << "client socket read error: ";
			message << strerror(errnoCode);
			message << " (errno " << errnoCode << ")";
			disconnectWithError(client, message.str());
		}
	}


	void onClientBodyBufferData(const ClientPtr &client, const char *data, size_t size, const FileBackedPipe::ConsumeCallback &consumed) {
		if (!client->connected()) {
			return;
		}

		switch (client->state) {
		case Client::FORWARDING_BODY_TO_APP:
			state_forwardingBodyToApp_onClientBodyBufferData(client, data, size, consumed);
			break;
		default:
			abort();
		}
	}

	void onClientBodyBufferError(const ClientPtr &client, int errorCode) {
		if (!client->connected()) {
			return;
		}

		stringstream message;
		message << "client body buffer error: ";
		message << strerror(errorCode);
		message << " (errno " << errorCode << ")";
		disconnectWithError(client, message.str());
	}

	void onClientBodyBufferEnd(const ClientPtr &client) {
		if (!client->connected()) {
			return;
		}

		switch (client->state) {
		case Client::FORWARDING_BODY_TO_APP:
			state_forwardingBodyToApp_onClientBodyBufferEnd(client);
			break;
		default:
			abort();
		}
	}

	void onClientBodyBufferCommit(const ClientPtr &client) {
		if (!client->connected()) {
			return;
		}

		switch (client->state) {
		case Client::BUFFERING_REQUEST_BODY:
			state_bufferingRequestBody_onClientBodyBufferCommit(client);
			break;
		default:
			abort();
		}
	}

	void onAppOutputWritable(const ClientPtr &client) {
		if (!client->connected()) {
			return;
		}

		switch (client->state) {
		case Client::SENDING_HEADER_TO_APP:
			state_sendingHeaderToApp_onAppOutputWritable(client);
			break;
		case Client::FORWARDING_BODY_TO_APP:
			state_forwardingBodyToApp_onAppOutputWritable(client);
			break;
		default:
			abort();
		}
	}


	void onTimeout(const ClientPtr &client) {
		if (!client->connected()) {
			return;
		}

		switch (client->state) {
		case Client::BEGIN_READING_CONNECT_PASSWORD:
		case Client::STILL_READING_CONNECT_PASSWORD:
			disconnectWithError(client, "no connect password received within timeout");
			break;
		default:
			disconnectWithError(client, "timeout");
			break;
		}
	}


	/*****************************************************
	 * COMPONENT: client -> application plumbing
	 *
	 * The following code implements forwarding data from
	 * the client to the application. Code is seperated
	 * by client state.
	 *****************************************************/


	/******* State: BEGIN_READING_CONNECT_PASSWORD *******/

	void checkConnectPassword(const ClientPtr &client, const char *data, unsigned int len) {
		RH_TRACE(client, 2, "Given connect password: \"" << cEscapeString(StaticString(data, len)) << "\"");
		if (StaticString(data, len) == options.requestSocketPassword) {
			RH_TRACE(client, 2, "Connect password is correct; reading header");
			client->state = Client::READING_HEADER;
			client->freeBufferedConnectPassword();
			client->timeoutTimer.stop();
		} else {
			disconnectWithError(client, "wrong connect password");
		}
	}

	size_t state_beginReadingConnectPassword_onClientData(const ClientPtr &client, const char *data, size_t size) {
		if (size >= options.requestSocketPassword.size()) {
			checkConnectPassword(client, data, options.requestSocketPassword.size());
			return options.requestSocketPassword.size();
		} else {
			client->bufferedConnectPassword.data = (char *) malloc(options.requestSocketPassword.size());
			client->bufferedConnectPassword.alreadyRead = size;
			memcpy(client->bufferedConnectPassword.data, data, size);
			client->state = Client::STILL_READING_CONNECT_PASSWORD;
			return size;
		}
	}


	/******* State: STILL_READING_CONNECT_PASSWORD *******/

	size_t state_stillReadingConnectPassword_onClientData(const ClientPtr &client, const char *data, size_t size) {
		size_t consumed = std::min<size_t>(size,
			options.requestSocketPassword.size() -
				client->bufferedConnectPassword.alreadyRead);
		memcpy(client->bufferedConnectPassword.data + client->bufferedConnectPassword.alreadyRead,
			data, consumed);
		client->bufferedConnectPassword.alreadyRead += consumed;
		if (client->bufferedConnectPassword.alreadyRead == options.requestSocketPassword.size()) {
			checkConnectPassword(client, client->bufferedConnectPassword.data,
				options.requestSocketPassword.size());
		}
		return consumed;
	}


	/******* State: READING_HEADER *******/

	bool modifyClientHeaders(const ClientPtr &client) {
		ScgiRequestParser &parser = client->scgiParser;
		ScgiRequestParser::HeaderMap &map = parser.getMap();
		ScgiRequestParser::iterator it, end = map.end();
		bool modified = false;

		/* The Rack spec specifies that HTTP_CONTENT_LENGTH and HTTP_CONTENT_TYPE must
		 * not exist and that their respective non-HTTP_ versions should exist instead.
		 */

		if ((it = map.find("HTTP_CONTENT_LENGTH")) != end) {
			if (map.find("CONTENT_LENGTH") == end) {
				map["CONTENT_LENGTH"] = it->second;
				map.erase("HTTP_CONTENT_LENGTH");
			} else {
				map.erase(it);
			}
			modified = true;
		}

		if ((it = map.find("HTTP_CONTENT_TYPE")) != end) {
			if (map.find("CONTENT_TYPE") == end) {
				map["CONTENT_TYPE"] = it->second;
				map.erase("HTTP_CONTENT_TYPE");
			} else {
				map.erase(it);
			}
			modified = true;
		}

		return modified;
	}

	size_t state_readingHeader_onClientData(const ClientPtr &client, const char *data, size_t size) {
		ScgiRequestParser &parser = client->scgiParser;
		size_t consumed = parser.feed(data, size);
		if (!parser.acceptingInput()) {
			if (parser.getState() == ScgiRequestParser::ERROR) {
				if (parser.getErrorReason() == ScgiRequestParser::LIMIT_REACHED) {
					disconnectWithError(client, "SCGI header too large");
				} else {
					disconnectWithError(client, "invalid SCGI header");
				}
				return consumed;
			}

			bool modified = modifyClientHeaders(client);
			/* TODO: in case the headers are not modified, we only need to rebuild the header data
			 * right now because the scgiParser buffer is invalidated as soon as onClientData exits.
			 * We should figure out a way to not copy anything if we can do everything before
			 * onClientData exits.
			 */
			parser.rebuildData(modified);

			if (getBoolOption(client, "PASSENGER_BUFFERING")) {
				RH_TRACE(client, 3, "Valid SCGI header; buffering request body");
				client->state = Client::BUFFERING_REQUEST_BODY;
				client->requestBodyIsBuffered = true;
			} else {
				RH_TRACE(client, 3, "Valid SCGI header; not buffering request body; checking out session");
				client->clientInput->stop();
				checkoutSession(client);
			}
		}
		return consumed;
	}


	/******* State: BUFFERING_REQUEST_BODY *******/

	void state_bufferingRequestBody_verifyInvariants(const ClientPtr &client) const {
		assert(client->requestBodyIsBuffered);
		assert(!client->clientBodyBuffer->isStarted());
	}

	size_t state_bufferingRequestBody_onClientData(const ClientPtr &client, const char *data, size_t size) {
		state_bufferingRequestBody_verifyInvariants(client);
		assert(!client->clientBodyBuffer->isCommittingToDisk());

		if (!client->clientBodyBuffer->write(data, size)) {
			// The pipe cannot write the data to disk quickly enough, so
			// suspend reading from the client until the pipe is done.
			client->backgroundOperations++; // TODO: figure out whether this is necessary
			client->clientInput->stop();
		}
		return size;
	}

	void state_bufferingRequestBody_onClientEof(const ClientPtr &client) {
		state_bufferingRequestBody_verifyInvariants(client);

		RH_TRACE(client, 3, "Done buffering request body; checking out session");
		client->clientBodyBuffer->end();
		checkoutSession(client);
	}

	void state_bufferingRequestBody_onClientBodyBufferCommit(const ClientPtr &client) {
		// Now that the pipe has committed the data to disk
		// resume reading from the client socket.
		state_bufferingRequestBody_verifyInvariants(client);
		assert(!client->clientInput->isStarted());
		client->backgroundOperations--;
		client->clientInput->start();
	}


	/******* State: CHECKING_OUT_SESSION *******/

	void state_checkingOutSession_verifyInvariants(const ClientPtr &client) {
		assert(!client->clientInput->isStarted());
		assert(!client->clientBodyBuffer->isStarted());
	}

	static void fillPoolOption(const ClientPtr &client, StaticString &field, const StaticString &name) {
		ScgiRequestParser::const_iterator it = client->scgiParser.getHeaderIterator(name);
		if (it != client->scgiParser.end()) {
			field = it->second;
		}
	}

	static void fillPoolOption(const ClientPtr &client, bool &field, const StaticString &name) {
		ScgiRequestParser::const_iterator it = client->scgiParser.getHeaderIterator(name);
		if (it != client->scgiParser.end()) {
			field = it->second == "true";
		}
	}

	void checkoutSession(const ClientPtr &client) {
		Options &options = client->options;

		fillPoolOption(client, options.appRoot, "PASSENGER_APP_ROOT");
		fillPoolOption(client, options.appType, "PASSENGER_APP_TYPE");
		fillPoolOption(client, options.spawnMethod, "PASSENGER_SPAWN_METHOD");
		fillPoolOption(client, options.startCommand, "PASSENGER_START_COMMAND");
		fillPoolOption(client, options.loadShellEnvvars, "PASSENGER_LOAD_SHELL_ENVVARS");
		// TODO

		RH_TRACE(client, 2, "Checking out session: appRoot=" << options.appRoot);
		client->state = Client::CHECKING_OUT_SESSION;
		pool->asyncGet(client->options, boost::bind(&RequestHandler::sessionCheckedOut,
			this, client, _1, _2));
		if (!client->sessionCheckedOut) {
			client->backgroundOperations++;
		}
	}

	void sessionCheckedOut(ClientPtr client, const SessionPtr &session, const ExceptionPtr &e) {
		if (!pthread_equal(pthread_self(), libev->getCurrentThread())) {
			libev->runAsync(boost::bind(&RequestHandler::sessionCheckedOut_real, this,
				client, session, e));
		} else {
			sessionCheckedOut_real(client, session, e);
		}
	}

	void sessionCheckedOut_real(ClientPtr client, const SessionPtr &session, const ExceptionPtr &e) {
		if (!client->connected()) {
			return;
		}

		state_checkingOutSession_verifyInvariants(client);
		client->backgroundOperations--;
		client->sessionCheckedOut = true;

		if (e != NULL) {
			try {
				shared_ptr<SpawnException> e2 = dynamic_pointer_cast<SpawnException>(e);
				if (e2->getErrorPage().empty()) {
					RH_WARN(client, "Cannot checkout session. " << e2->what());
					writeErrorResponse(client, e2->what());
				} else {
					RH_WARN(client, "Cannot checkout session. " << e2->what() <<
						"\nError page:\n" << e2->getErrorPage());
					writeErrorResponse(client, e2->getErrorPage(), e2.get());
				}
			} catch (const bad_cast &) {
				RH_WARN(client, "Cannot checkout session; error messages can be found above");
				writeErrorResponse(client, e->what());
			}
		} else {
			RH_TRACE(client, 3, "Session checked out: pid=" << session->getPid() <<
				", gupid=" << session->getGupid());
			client->session = session;
			initiateSession(client);
		}
	}

	void initiateSession(const ClientPtr &client) {
		assert(client->state == Client::CHECKING_OUT_SESSION);
		client->sessionCheckoutTry++;
		try {
			client->session->initiate();
		} catch (const SystemException &e2) {
			if (client->sessionCheckoutTry < 10) {
				RH_TRACE(client, 2, "Error checking out session (" << e2.what() <<
					"); retrying (attempt " << client->sessionCheckoutTry << ")");
				client->sessionCheckedOut = false;
				pool->asyncGet(client->options,
					boost::bind(&RequestHandler::sessionCheckedOut,
						this, client, _1, _2));
				if (!client->sessionCheckedOut) {
					client->backgroundOperations++;
				}
			} else {
				disconnectWithError(client, "could not initiate a session");
			}
			return;
		}
		
		client->appInput->reset(libev.get(), client->session->fd());
		client->appInput->start();
		client->appOutputWatcher.set(libev->getLoop());
		client->appOutputWatcher.set(client->session->fd());
		sendHeaderToApp(client);
	}


	/******* State: SENDING_HEADER_TO_APP *******/

	static StaticString makeStaticStringWithNull(const char *data) {
		return StaticString(data, strlen(data) + 1);
	}

	static StaticString makeStaticStringWithNull(const string &data) {
		return StaticString(data.c_str(), data.size() + 1);
	}

	void state_sendingHeaderToApp_verifyInvariants(const ClientPtr &client) {
		assert(!client->clientInput->isStarted());
		assert(!client->clientBodyBuffer->isStarted());
	}

	void sendHeaderToApp(const ClientPtr &client) {
		assert(!client->clientInput->isStarted());
		assert(!client->clientBodyBuffer->isStarted());

		RH_TRACE(client, 2, "Sending headers to application");

		char sizeField[sizeof(uint32_t)];
		StaticString data[] = {
			StaticString(sizeField, sizeof(uint32_t)),
			client->scgiParser.getHeaderData(),

			makeStaticStringWithNull("PASSENGER_CONNECT_PASSWORD"),
			makeStaticStringWithNull(client->session->getConnectPassword())
		};

		uint32_t dataSize = 0;
		for (unsigned int i = 1; i < sizeof(data) / sizeof(StaticString); i++) {
			dataSize += (uint32_t) data[i].size();
		}
		Uint32Message::generate(sizeField, dataSize);

		ssize_t ret = gatheredWrite(client->session->fd(), data,
			sizeof(data) / sizeof(StaticString), client->appOutputBuffer);
		if (ret == -1 && errno != EAGAIN) {
			disconnectWithAppSocketWriteError(client, errno);
		} else if (!client->appOutputBuffer.empty()) {
			client->state = Client::SENDING_HEADER_TO_APP;
			client->appOutputWatcher.start();
		} else {
			sendBodyToApp(client);
		}
	}

	void state_sendingHeaderToApp_onAppOutputWritable(const ClientPtr &client) {
		state_sendingHeaderToApp_verifyInvariants(client);

		ssize_t ret = gatheredWrite(client->session->fd(), NULL, 0, client->appOutputBuffer);
		if (ret == -1) {
			if (errno != EAGAIN && errno != EPIPE) {
				disconnectWithAppSocketWriteError(client, errno);
			}
		} else if (client->appOutputBuffer.empty()) {
			client->appOutputWatcher.stop();
			sendBodyToApp(client);
		}
	}


	/******* State: FORWARDING_BODY_TO_APP *******/

	void state_forwardingBodyToApp_verifyInvariants(const ClientPtr &client) {
		assert(client->state == Client::FORWARDING_BODY_TO_APP);
	}

	void sendBodyToApp(const ClientPtr &client) {
		assert(client->appOutputBuffer.empty());
		assert(!client->clientBodyBuffer->isStarted());
		assert(!client->clientInput->isStarted());
		assert(!client->appOutputWatcher.is_active());

		RH_TRACE(client, 2, "Sending body to application");

		client->state = Client::FORWARDING_BODY_TO_APP;
		if (client->requestBodyIsBuffered) {
			client->clientBodyBuffer->start();
		} else {
			client->clientInput->start();
		}
	}


	size_t state_forwardingBodyToApp_onClientData(const ClientPtr &client,
		const char *data, size_t size)
	{
		state_forwardingBodyToApp_verifyInvariants(client);
		assert(!client->requestBodyIsBuffered);

		ssize_t ret = syscalls::write(client->session->fd(), data, size);
		if (ret == -1) {
			if (errno == EAGAIN) {
				// App is not ready yet to receive this data. Try later
				// when the app socket is writable.
				client->clientInput->stop();
				client->appOutputWatcher.start();
			} else if (errno == EPIPE) {
				// Client will be disconnected after response forwarding is done.
				client->clientInput->stop();
				syscalls::shutdown(client->fd, SHUT_RD);
			} else {
				disconnectWithAppSocketWriteError(client, errno);
			}
			return 0;
		} else {
			return ret;
		}
	}

	void state_forwardingBodyToApp_onClientEof(const ClientPtr &client) {
		state_forwardingBodyToApp_verifyInvariants(client);
		assert(!client->requestBodyIsBuffered);

		RH_TRACE(client, 2, "End of (unbuffered) client body reached; done sending data to application");
		client->clientInput->stop();
		syscalls::shutdown(client->session->fd(), SHUT_WR);
	}

	void state_forwardingBodyToApp_onAppOutputWritable(const ClientPtr &client) {
		state_forwardingBodyToApp_verifyInvariants(client);
		assert(!client->requestBodyIsBuffered);

		client->clientInput->start();
		client->clientOutputWatcher.stop();
	}


	void state_forwardingBodyToApp_onClientBodyBufferData(const ClientPtr &client,
		const char *data, size_t size, const FileBackedPipe::ConsumeCallback &consumed)
	{
		state_forwardingBodyToApp_verifyInvariants(client);
		assert(client->requestBodyIsBuffered);

		ssize_t ret = syscalls::write(client->session->fd(), data, size);
		if (ret == -1) {
			if (errno == EAGAIN) {
				// App is not ready yet to receive this data. Try later
				// when the app socket is writable.
				client->clientBodyBuffer->stop();
				client->appOutputWatcher.start();
			} else if (errno == EPIPE) {
				// Client will be disconnected after response forwarding is done.
				syscalls::shutdown(client->fd, SHUT_RD);
			} else {
				disconnectWithAppSocketWriteError(client, errno);
			}
			consumed(0, true);
		} else {
			consumed(ret, false);
		}
	}

	void state_forwardingBodyToApp_onClientBodyBufferEnd(const ClientPtr &client) {
		state_forwardingBodyToApp_verifyInvariants(client);
		assert(client->requestBodyIsBuffered);

		RH_TRACE(client, 2, "End of (buffered) client body reached; done sending data to application");
		syscalls::shutdown(client->session->fd(), SHUT_WR);
	}


public:
	// For unit testing purposes.
	unsigned int connectPasswordTimeout; // milliseconds

	RequestHandler(const SafeLibevPtr &_libev, const FileDescriptor &_requestSocket,
		const PoolPtr &_pool, const AgentOptions &_options)
		: libev(_libev),
		  requestSocket(_requestSocket),
		  pool(_pool),
		  options(_options),
		  resourceLocator(_options.passengerRoot)
	{
		accept4Available = true;
		connectPasswordTimeout = 15000;

		requestSocketWatcher.set(_requestSocket, ev::READ);
		requestSocketWatcher.set(_libev->getLoop());
		requestSocketWatcher.set<RequestHandler, &RequestHandler::onAcceptable>(this);
		requestSocketWatcher.start();
	}

	template<typename Stream>
	void inspect(Stream &stream) const {
		stream << clients.size() << " clients:\n";
		HashMap<int, ClientPtr>::const_iterator it;
		for (it = clients.begin(); it != clients.end(); it++) {
			const ClientPtr &client = it->second;
			stream << "  Client " << client->fd << ":\n";
			client->inspect(stream);
		}
	}
};


} // namespace Passenger

#endif /* _PASSENGER_REQUEST_HANDLER_H_ */
