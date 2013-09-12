#  Phusion Passenger - https://www.phusionpassenger.com/
#  Copyright (c) 2010-2013 Phusion
#
#  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
#
#  Permission is hereby granted, free of charge, to any person obtaining a copy
#  of this software and associated documentation files (the "Software"), to deal
#  in the Software without restriction, including without limitation the rights
#  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
#  copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice shall be included in
#  all copies or substantial portions of the Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
#  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
#  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
#  THE SOFTWARE.

require 'phusion_passenger'

module PhusionPassenger
	PASSENGER_ANALYTICS_WEB_LOG = "PASSENGER_ANALYTICS_WEB_LOG".freeze
	PASSENGER_TXN_ID            = "PASSENGER_TXN_ID".freeze
	PASSENGER_UNION_STATION_KEY = "UNION_STATION_KEY".freeze
	RACK_HIJACK_IO              = "rack.hijack_io".freeze

	# Constants shared between the C++ and Ruby codebase. The C++ Constants.h
	# is automatically generated by the build system from the following
	# definitions.
	module SharedConstants
		# Default config values
		DEFAULT_LOG_LEVEL = 0
		DEFAULT_RUBY = "ruby"
		DEFAULT_PYTHON = "python"
		DEFAULT_MAX_POOL_SIZE = 6
		DEFAULT_POOL_IDLE_TIME = 300
		DEFAULT_START_TIMEOUT = 90_000
		DEFAULT_MAX_INSTANCES_PER_APP = 0
		DEFAULT_WEB_APP_USER = "nobody"
		DEFAULT_CONCURRENCY_MODEL = "process"
		DEFAULT_THREAD_COUNT = 1
		DEFAULT_ANALYTICS_LOG_USER = DEFAULT_WEB_APP_USER
		DEFAULT_ANALYTICS_LOG_GROUP = ""
		DEFAULT_ANALYTICS_LOG_PERMISSIONS = "u=rwx,g=rx,o=rx"
		DEFAULT_UNION_STATION_GATEWAY_ADDRESS = "gateway.unionstationapp.com"
		DEFAULT_UNION_STATION_GATEWAY_PORT = 443

		# Size limits
		MESSAGE_SERVER_MAX_USERNAME_SIZE = 100
		MESSAGE_SERVER_MAX_PASSWORD_SIZE = 100
		POOL_HELPER_THREAD_STACK_SIZE = 1024 * 256

		# Time limits
		PROCESS_SHUTDOWN_TIMEOUT = 60 # In seconds
		PROCESS_SHUTDOWN_TIMEOUT_DISPLAY = "1 minute"

		# Versions
		PASSENGER_VERSION = PhusionPassenger::VERSION_STRING
		SERVER_INSTANCE_DIR_STRUCTURE_MAJOR_VERSION = 1
		SERVER_INSTANCE_DIR_STRUCTURE_MINOR_VERSION = 0
		SERVER_INSTANCE_DIR_GENERATION_STRUCTURE_MAJOR_VERSION = 2
		SERVER_INSTANCE_DIR_GENERATION_STRUCTURE_MINOR_VERSION = 0

		# Misc
		FEEDBACK_FD = 3
		STANDALONE_NGINX_CONFIGURE_OPTIONS =
			"--with-cc-opt='-Wno-error' " <<
			"--without-http_fastcgi_module " <<
			"--without-http_scgi_module " <<
			"--without-http_uwsgi_module " <<
			"--with-http_gzip_static_module " <<
			"--with-http_stub_status_module"
	end

	SharedConstants.constants.each do |name|
		const_set(name, SharedConstants.const_get(name)) unless const_defined? name
	end
end
