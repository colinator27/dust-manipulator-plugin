/*
dust-manipulator-plugin
Copyright (C) 2025 colinator27

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/vec4.h>

#include <asio.hpp>

#include <unordered_set>
#include <mutex>
#include <vector>
#include <thread>

#define MESSAGE_CODE_HELLO 0x55541000
#define MESSAGE_CODE_HELLO_ACK 0x55541001
#define MESSAGE_CODE_GOODBYE 0x55542000
#define MESSAGE_CODE_SCREENSHOT 0x55544000
#define MESSAGE_CODE_SCREENSHOT_START_DELAY 0x55545000
#define MESSAGE_CODE_SCREENSHOT_MODE 0x55546000
#define MESSAGE_CODE_HOTKEY_BASE 0x55548000

#pragma pack(push, 1)
typedef struct message_header 
{
	uint32_t message_code;
	uint32_t screenshot_has_more;
	uint32_t screenshot_width;
	uint32_t screenshot_height;
	uint32_t screenshot_stride;
	uint32_t screenshot_bits_per_pixel;
} message_header;
#pragma pack(pop)

class MessageToSend
{
public:
	size_t m_Length;
	const char* m_Data;

	MessageToSend()
	{
		m_Data = nullptr;
		m_Length = 0;
	}

	MessageToSend(const char *data, size_t length)
	{
		m_Data = data;
		m_Length = length;
	}
};

class Connection 
{
private:
	bool m_Started = false;
	bool m_Acknowledged = false;

	int m_Port = 0;

	std::vector<MessageToSend> m_SendMessages;
	std::mutex m_SendMessageMutex;
	std::mutex m_ConnectionThreadMutex;
	std::mutex m_ScreenshotConfigMutex;
	std::thread m_ConnectionThread;
	bool m_ShutdownThread = false;

	uint8_t m_ReceiveBuffer[8];
	uint32_t m_ScreenshotStartDelay;
	bool m_IsSingleScreenshotOnly;

	void send_message(const char* data, size_t length)
	{
		// Don't send any messages when shutting down
		if (m_ShutdownThread)
		{
			// Free memory from the message...
			bfree((void*)data);
			return;
		}

		// Add message to sending queue for the connection thread to pick up
		m_SendMessageMutex.lock();
		m_SendMessages.emplace_back(data, length);
		m_SendMessageMutex.unlock();
	}

	void connection_thread()
	{
		// Perform initial connection
		obs_log(LOG_INFO, "attempting to connect to 127.0.0.1:%d", m_Port);
		asio::io_context ioContext;
		asio::ip::tcp::socket socket(ioContext);
		try 
		{
			socket.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), m_Port));
		}
		catch (std::exception& e)
		{
			obs_log(LOG_ERROR, "error connecting: %s", e.what());
			m_ShutdownThread = true;
			return;
		}

		// Send hello message
		message_header helloHeader = {0};
		helloHeader.message_code = htonl(MESSAGE_CODE_HELLO);
		try
		{
			asio::write(socket, asio::buffer(&helloHeader, sizeof(message_header)));
		}
		catch (std::exception& e)
		{
			obs_log(LOG_ERROR, "failed to send hello message: %s", e.what());
			m_ShutdownThread = true;
			return;
		}

		// Wait for acknowledgment
		uint32_t acknowledgeCode = 0;
		try
		{
			size_t acknowledgeLength = asio::read(socket, asio::buffer(&acknowledgeCode, sizeof(acknowledgeCode)));
			if (acknowledgeLength != sizeof(acknowledgeCode))
			{
				throw std::runtime_error("unexpected acknowledgment length");
			}
		}
		catch (std::exception& e)
		{
			obs_log(LOG_ERROR, "failed to read from socket: %s", e.what());
			m_ShutdownThread = true;
			return;
		}
		acknowledgeCode = ntohl(acknowledgeCode);
		if (acknowledgeCode != MESSAGE_CODE_HELLO_ACK)
		{
			obs_log(LOG_ERROR, "did not receive valid acknowledgment, got %d", acknowledgeCode);
			m_ShutdownThread = true;
			return;
		}
		m_Acknowledged = true;
		obs_log(LOG_INFO, "received valid acknowledgment");

		// Wait for messages to send
		bool anyRemainingMessages = false;
		while (!m_ShutdownThread || anyRemainingMessages)
		{
			// Check if connection is still open
			if (!socket.is_open())
			{
				obs_log(LOG_ERROR, "socket closed unexpectedly");
				m_Acknowledged = false;
				m_ShutdownThread = true;
				return;
			}

			// Look for any message data to send
			bool messagesToSend = false;
			do
			{
				// Find a message in queue
				m_SendMessageMutex.lock();
				MessageToSend message;
				if (!m_SendMessages.empty())
				{
					message = m_SendMessages.front();
					m_SendMessages.erase(m_SendMessages.begin());
					messagesToSend = !m_SendMessages.empty();
				}
				else
				{
					messagesToSend = false;
				}
				m_SendMessageMutex.unlock();

				// Send message, if there was any in the queue
				if (message.m_Length > 0)
				{
					try
					{
						asio::write(socket, asio::buffer(message.m_Data, message.m_Length));
					}
					catch (std::exception& e)
					{
						obs_log(LOG_ERROR, "failed to send packet: %s", e.what());
						m_Acknowledged = false;
						m_ShutdownThread = true;
						return;
					}

					bfree((void*)(message.m_Data));
				}
			}
			while (messagesToSend);

			if (!m_ShutdownThread)
			{
				// Check for any messages to read
				try
				{
					// Read non-blocking specifically
					socket.non_blocking(true);
					asio::error_code err;
					size_t num_bytes = socket.receive(asio::buffer(m_ReceiveBuffer, sizeof(m_ReceiveBuffer)), (asio::socket_base::message_flags)0, err);
					socket.non_blocking(false);

					// Throw error if one occurs
					if (num_bytes == 0 && err.value() != asio::error::would_block)
					{
						throw std::runtime_error(err.message());
					}

					// Process received data
					if (num_bytes > 0)
					{
						// Ensure entire buffer is filled
						if (num_bytes != 8)
						{
							throw std::runtime_error("unexpected number of bytes received");
						}

						// Check message code
						uint32_t message_code = ntohl(*(uint32_t*)m_ReceiveBuffer);
						if (message_code != MESSAGE_CODE_SCREENSHOT_START_DELAY &&
						    message_code != MESSAGE_CODE_SCREENSHOT_MODE)
						{
							throw std::runtime_error("unexpected message code received");
						}

						if (message_code == MESSAGE_CODE_SCREENSHOT_MODE)
						{
							// Adjust mode
							m_ScreenshotConfigMutex.lock();
							m_IsSingleScreenshotOnly = ntohl(*(uint32_t *)(m_ReceiveBuffer + 4)) != 0;
							m_ScreenshotConfigMutex.unlock();
						}
						else
						{
							// Adjust start delay
							m_ScreenshotConfigMutex.lock();
							m_ScreenshotStartDelay = ntohl(*(uint32_t*)(m_ReceiveBuffer + 4));
							m_ScreenshotConfigMutex.unlock();
						}
					}
				}
				catch (std::exception& e)
				{
					obs_log(LOG_ERROR, "failed to read from socket: %s", e.what());
					m_Acknowledged = false;
					m_ShutdownThread = true;
					return;
				}

				// Sleep for a bit while there's no messages (presumably), to not waste CPU
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}

			if (m_ShutdownThread)
			{
				// Check for any remaining messages when shutting down
				m_SendMessageMutex.lock();
				anyRemainingMessages = !m_SendMessages.empty();
				m_SendMessageMutex.unlock();
			}
		}
		obs_log(LOG_INFO, "connection thread completed");
	}

public:
	void start(uint32_t port)
	{
		if (m_Started)
		{
			stop();
		}

		m_ConnectionThreadMutex.lock();
		if (m_Started)
		{
			m_ConnectionThreadMutex.unlock();
			return;
		}
		m_Port = port;
		m_Started = true;
		m_Acknowledged = false;
		m_ShutdownThread = false;
		m_ConnectionThread = std::thread(&Connection::connection_thread, this);
		m_ConnectionThreadMutex.unlock();
	}

	void stop()
	{
		if (m_Started && m_Acknowledged && !m_ShutdownThread)
		{
			obs_log(LOG_INFO, "TCP session closing: sending goodbye message to server");

			message_header* goodbyeHeader = (message_header*)bzalloc(sizeof(message_header));
			if (!goodbyeHeader)
			{
				obs_log(LOG_ERROR, "failed to allocate header for goodbye message");
				return;
			}
			goodbyeHeader->message_code = htonl(MESSAGE_CODE_GOODBYE);
			send_message((char*)goodbyeHeader, sizeof(message_header));
		}

		// Shut down and wait for thread to finish
		m_ShutdownThread = true;
		m_ConnectionThreadMutex.lock();
		if (m_ConnectionThread.joinable())
		{
			m_ConnectionThread.join();
		}
		m_ConnectionThreadMutex.unlock();
		m_SendMessageMutex.lock();
		m_SendMessages.clear();
		m_SendMessageMutex.unlock();
		m_ShutdownThread = false;
		m_Started = false;
		m_Acknowledged = false;
	}

	void send_hotkey(int hotkey_number)
	{
		if (!m_Started || !m_Acknowledged || m_ShutdownThread)
		{
			obs_log(LOG_WARNING, "couldn't send hotkey; no active connection");
			return;
		}
		
		message_header* header = (message_header*)bzalloc(sizeof(message_header));
		if (!header)
		{
			obs_log(LOG_ERROR, "failed to allocate header for hotkey message");
			return;
		}
		header->message_code = htonl(MESSAGE_CODE_HOTKEY_BASE + hotkey_number);
		send_message((char *)header, sizeof(message_header));
	}

	void send_screenshot(uint8_t* data, uint32_t width, uint32_t height, uint32_t stride, bool has_more)
	{
		if (!m_Started || !m_Acknowledged || m_ShutdownThread)
		{
			obs_log(LOG_WARNING, "couldn't send screenshot; no active connection");
			return;
		}
		
		uint32_t len = stride * height;
		char* final_buffer = (char*)bmalloc(sizeof(message_header) + len);
		if (!final_buffer)
		{
			obs_log(LOG_ERROR, "failed to allocate memory for final screenshot buffer");
			return;
		}
		message_header *header = (message_header *)final_buffer;
		header->message_code = htonl(MESSAGE_CODE_SCREENSHOT);
		header->screenshot_has_more = htonl(has_more ? 1 : 0);
		header->screenshot_width = htonl(width);
		header->screenshot_height = htonl(height);
		header->screenshot_stride = htonl(stride);
		header->screenshot_bits_per_pixel = htonl(32);
		memcpy(final_buffer + sizeof(message_header), data, len);
		send_message(final_buffer, sizeof(message_header) + len);
	}

	uint32_t get_screenshot_start_delay_ms()
	{
		m_ScreenshotConfigMutex.lock();
		uint32_t res = m_ScreenshotStartDelay;
		m_ScreenshotConfigMutex.unlock();
		return res;
	}

	bool get_is_single_screenshot_only()
	{
		m_ScreenshotConfigMutex.lock();
		bool res = m_IsSingleScreenshotOnly;
		m_ScreenshotConfigMutex.unlock();
		return res;
	}

	void connect_if_not_already(uint32_t port)
	{
		if (!m_Started || m_ShutdownThread)
		{
			start(port);
		}
	}

	~Connection()
	{
		stop();
	}
};

typedef struct dust_manipulator_filter 
{
	obs_source_t* source;
	gs_texrender_t* texrender;
	gs_stagesurf_t* stagesurface;

	Connection* connection;

	uint32_t source_width;
	uint32_t source_height;

	bool is_active;

	bool is_taking_screenshots;
	uint32_t screenshot_counter;
	uint32_t max_screenshot_counter;
	uint32_t screenshot_width;
	uint32_t screenshot_height;

	float screenshot_start_countdown;
	bool screenshot_is_single_only;

	uint32_t output_port_number;
} dust_manipulator_filter;

obs_hotkey_id s_Hotkey1;
obs_hotkey_id s_Hotkey2;
obs_hotkey_id s_Hotkey3;
obs_hotkey_id s_Hotkey4;
bool s_HotkeysInitialized = false;
std::unordered_set<dust_manipulator_filter*> s_ActiveFilters = {};

typedef struct vec4 vec4;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static const char* filter_get_name(void* unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Dust Manipulator");
}

static void* filter_create(obs_data_t* settings, obs_source_t* source)
{
	dust_manipulator_filter* s = (dust_manipulator_filter*)bzalloc(sizeof(dust_manipulator_filter));
	s->source = source;
	s->connection = new Connection();

	obs_source_update(source, settings);

	s_ActiveFilters.insert(s);

	return s;
}

static void filter_destroy(void* data)
{
	dust_manipulator_filter* s = (dust_manipulator_filter*)data;

	delete s->connection;
	s->connection = NULL;

	s_ActiveFilters.erase(s);

	obs_enter_graphics();
	if (s->texrender)
	{
		gs_texrender_destroy(s->texrender);
		s->texrender = NULL;
	}
	if (s->stagesurface) 
	{
		gs_stagesurface_destroy(s->stagesurface);
		s->stagesurface = NULL;
	}
	obs_leave_graphics();

	bfree(s);
}

static void filter_update(void* data, obs_data_t* settings)
{
	dust_manipulator_filter* s = (dust_manipulator_filter*)data;

	s->max_screenshot_counter = (uint32_t)obs_data_get_int(settings, "max_screenshot_count");

	uint32_t last_screenshot_width = s->screenshot_width;
	uint32_t last_screenshot_height = s->screenshot_height;
	s->screenshot_width = (uint32_t)obs_data_get_int(settings, "screenshot_width");
	s->screenshot_height = (uint32_t)obs_data_get_int(settings, "screenshot_height");
	if ((last_screenshot_width != s->screenshot_width || last_screenshot_height != s->screenshot_height) && s->stagesurface)
	{
		// Invalidate stagessurface when width/height change
		obs_enter_graphics();
		gs_stagesurface_destroy(s->stagesurface);
		s->stagesurface = NULL;
		obs_leave_graphics();
	}

	uint32_t last_port = s->output_port_number;
	s->output_port_number = (uint32_t)obs_data_get_int(settings, "output_port_number");

	// Attempt a new TCP connection if port changed and is valid
	if (last_port != s->output_port_number && s->output_port_number > 0)
	{
		s->connection->start(s->output_port_number);
	}
}

static obs_properties_t *filter_properties(void*)
{
	// Filter settings
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_int(props, "max_screenshot_count", obs_module_text("Number of screenshots to take"), 1, 60, 1);
	obs_properties_add_int(props, "screenshot_width", obs_module_text("Screenshot width"), 1, 1280, 1);
	obs_properties_add_int(props, "screenshot_height", obs_module_text("Screenshot height"), 1, 720, 1);
	obs_properties_add_int(props, "output_port_number", obs_module_text("Local port number to send data to"), 1024, 65535, 1);
	return props;
}

static void filter_get_defaults(obs_data_t *settings)
{
	// Default values for the filter settings
	obs_data_set_default_int(settings, "max_screenshot_count", 10);
	obs_data_set_default_int(settings, "screenshot_width", 640);
	obs_data_set_default_int(settings, "screenshot_height", 480);
	obs_data_set_default_int(settings, "output_port_number", 48654);

	obs_data_t *presets = obs_data_create();
	obs_data_set_default_obj(settings, "presets", presets);
	obs_data_release(presets);
}

static void filter_load(void*, obs_data_t* settings)
{
	// Load hotkeys along with filter information
	obs_data_array_t *hotkey_data = obs_data_get_array(settings, "start_hotkey");
	if (hotkey_data)
	{
		if (s_HotkeysInitialized)
		{
			obs_hotkey_load(s_Hotkey1, hotkey_data);
		}
		obs_data_array_release(hotkey_data);
	}

	hotkey_data = obs_data_get_array(settings, "hotkey_2");
	if (hotkey_data)
	{
		if (s_HotkeysInitialized)
		{
			obs_hotkey_load(s_Hotkey2, hotkey_data);
		}
		obs_data_array_release(hotkey_data);
	}

	hotkey_data = obs_data_get_array(settings, "hotkey_3");
	if (hotkey_data)
	{
		if (s_HotkeysInitialized)
		{
			obs_hotkey_load(s_Hotkey3, hotkey_data);
		}
		obs_data_array_release(hotkey_data);
	}

	hotkey_data = obs_data_get_array(settings, "hotkey_4");
	if (hotkey_data)
	{
		if (s_HotkeysInitialized)
		{
			obs_hotkey_load(s_Hotkey4, hotkey_data);
		}
		obs_data_array_release(hotkey_data);
	}
}

static void filter_save(void*, obs_data_t* settings)
{
	// Save hotkeys along with filter information
	if (s_HotkeysInitialized)
	{
		obs_data_array_t *hotkey_data = obs_hotkey_save(s_Hotkey1);
		if (hotkey_data)
		{
			obs_data_set_array(settings, "start_hotkey", hotkey_data);
			obs_data_array_release(hotkey_data);
		}
		
		hotkey_data = obs_hotkey_save(s_Hotkey2);
		if (hotkey_data)
		{
			obs_data_set_array(settings, "hotkey_2", hotkey_data);
			obs_data_array_release(hotkey_data);
		}

		hotkey_data = obs_hotkey_save(s_Hotkey3);
		if (hotkey_data) 
		{
			obs_data_set_array(settings, "hotkey_3", hotkey_data);
			obs_data_array_release(hotkey_data);
		}
		
		hotkey_data = obs_hotkey_save(s_Hotkey4);
		if (hotkey_data)
		{
			obs_data_set_array(settings, "hotkey_4", hotkey_data);
			obs_data_array_release(hotkey_data);
		}
	}
}

static void filter_activate(void *data)
{
	dust_manipulator_filter *s = (dust_manipulator_filter *)data;
	s->is_active = true;

	// Attempt TCP connection
	if (s->output_port_number > 0) 
	{
		s->connection->start(s->output_port_number);
	}
}

static void filter_deactivate(void *data)
{
	dust_manipulator_filter* s = (dust_manipulator_filter*)data;
	s->is_active = false;
	s->is_taking_screenshots = false;
	s->screenshot_counter = 0;

	// Stop TCP connection
	s->connection->stop();
}

static void filter_tick(void *data, float second)
{
	dust_manipulator_filter* s = (dust_manipulator_filter*)data;
	
	// Make sure the filter is assigned to a target
	obs_source_t *target = obs_filter_get_target(s->source);
	if (!target)
	{
		return;
	}

	// Update source width/height
	s->source_width = obs_source_get_base_width(target);
	s->source_height = obs_source_get_base_height(target);

	// Nothing else to do while not taking screenshots
	if (!s->is_taking_screenshots)
	{
		return;
	}

	// Handle start countdown
	if (s->screenshot_start_countdown > 0.0f)
	{
		s->screenshot_start_countdown -= second;
		if (s->screenshot_start_countdown > 0.0f)
		{
			return;
		}
	}

	// Take screenshot - enter graphics context
	obs_enter_graphics();

	// Prepare texrender and stagesurface if not already prepared
	if (!s->texrender)
	{
		s->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (!s->texrender)
		{
			obs_log(LOG_ERROR, "failed to create texrender");
			return;
		}
	}
	if (!s->stagesurface)
	{
		s->stagesurface = gs_stagesurface_create(s->screenshot_width, s->screenshot_height, GS_RGBA);
		if (!s->texrender)
		{
			obs_log(LOG_ERROR, "failed to create stagesurface");
			return;
		}
	}

	// Begin the texrender
	gs_texrender_reset(s->texrender);
	if (gs_texrender_begin(s->texrender, s->screenshot_width, s->screenshot_height)) 
	{
		// Use a black background color
		vec4 background_color;
		vec4_zero(&background_color);

		// Clear the background and set up orthographic perspective
		gs_clear(GS_CLEAR_COLOR, &background_color, 0.0f, 0);
		gs_ortho(0.0f, (float)s->source_width, 0.0f, (float)s->source_height, -100.0f, 100.0f);

		// Push a simple blend mode
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		// Render the video from the target
		obs_source_inc_showing(target);
		obs_source_video_render(target);
		obs_source_dec_showing(target);

		// Undo the blend mode and finish the texrender
		gs_blend_state_pop();
		gs_texrender_end(s->texrender);

		// Stage the texture to the stagesurface, and try to map its data
		gs_stage_texture(s->stagesurface, gs_texrender_get_texture(s->texrender));
		uint8_t *data;
		uint32_t linesize;
		if (gs_stagesurface_map(s->stagesurface, &data, &linesize)) 
		{
			// Send data over
			s->connection->send_screenshot(data, s->screenshot_width, s->screenshot_height, linesize, 
				s->screenshot_counter < (s->max_screenshot_counter - 1) && !s->screenshot_is_single_only);

			// Unmap the stagesurface
			gs_stagesurface_unmap(s->stagesurface);
		}
	}

	// Done taking screenshot - exit graphics context
	obs_leave_graphics();

	// Increment screenshot count, and stop if reached maximum
	s->screenshot_counter++;
	if (s->screenshot_counter >= s->max_screenshot_counter || s->screenshot_is_single_only)
	{
		s->is_taking_screenshots = false;
	}
}

static void filter_render(void* data, gs_effect_t*)
{
	// Passthrough - skip the filter
	dust_manipulator_filter* s = (dust_manipulator_filter*)data;
	obs_source_skip_video_filter(s->source);
}

static uint32_t filter_width(void *data)
{
	// Passthrough - use normal source width
	dust_manipulator_filter* s = (dust_manipulator_filter*)data;
	return s->source_width;
}

static uint32_t filter_height(void *data)
{
	// Passthrough - use normal source height
	dust_manipulator_filter* s = (dust_manipulator_filter*)data;
	return s->source_height;
}

static void filter_handle_hotkey(dust_manipulator_filter* s, int hotkey_number)
{
	// Make sure we're currently active
	if (!s->is_active)
	{
		return;
	}

	// If not currently connected, try to start a connection
	if (s->connection && s->output_port_number > 0) 
	{
		s->connection->connect_if_not_already(s->output_port_number);
	}

	// If we have a connection, send the hotkey message, and get current start delay
	float current_start_delay = 0.0f;
	bool current_is_single_only = true; // (prevent console spam by default)
	if (s->connection)
	{
		s->connection->send_hotkey(hotkey_number);
		current_start_delay = (float)s->connection->get_screenshot_start_delay_ms() / 1000.0f;
		current_is_single_only = s->connection->get_is_single_screenshot_only();
	}

	// Start taking screenshot(s) if the first hotkey is pressed
	if (hotkey_number == 0 && !s->is_taking_screenshots)
	{
		uint32_t num_screenshots = current_is_single_only ? 1 : s->max_screenshot_counter;

		// Subtract the half the number of frames we're taking for screenshots from the delay, approximately
		current_start_delay -= (1.0f / 30.0f) * ((float)num_screenshots * 0.5f);
		if (current_start_delay < 0.0f)
		{
			current_start_delay = 0.0f;
		}

		obs_log(LOG_INFO, "taking %d rapid screenshot(s) after %f delay and sending", num_screenshots, current_start_delay);
		s->screenshot_counter = 0;
		s->is_taking_screenshots = true;

		s->screenshot_start_countdown = current_start_delay;
		s->screenshot_is_single_only = current_is_single_only;
	}
}

static void handle_hotkey(void*, obs_hotkey_id id, obs_hotkey_t*, bool pressed)
{
	// Make sure hotkeys initialized and that it was pressed
	if (!s_HotkeysInitialized)
	{
		return;
	}
	if (!pressed)
	{
		return;
	}

	// Figure out which hotkey was pressed
	int hotkey_number;
	if (id == s_Hotkey1)
	{
		hotkey_number = 0;
	}
	else if (id == s_Hotkey2)
	{
		hotkey_number = 1;
	}
	else if (id == s_Hotkey3)
	{
		hotkey_number = 2;
	}
	else if (id == s_Hotkey4)
	{
		hotkey_number = 3;
	}
	else
	{
		return;
	}

	// Handle hotkey on all filters
	for (dust_manipulator_filter* s : s_ActiveFilters)
	{
		filter_handle_hotkey(s, hotkey_number);
	}
}

bool obs_module_load(void)
{
	// Register filter
	struct obs_source_info info = {0};
	info.id = "dust_manipulator_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = filter_get_name;
	info.create = filter_create;
	info.destroy = filter_destroy;
	info.update = filter_update;
	info.get_properties = filter_properties;
	info.get_defaults = filter_get_defaults;
	info.activate = filter_activate;
	info.deactivate = filter_deactivate;
	info.video_tick = filter_tick;
	info.video_render = filter_render;
	info.get_width = filter_width;
	info.get_height = filter_height;
	info.load = filter_load;
	info.save = filter_save;
	obs_register_source(&info);

	// Register hotkey
	s_Hotkey1 = obs_hotkey_register_frontend("dust_manipulator_filter.start", obs_module_text("Hotkey 1 (Take Screenshots)"), handle_hotkey, NULL);
	s_Hotkey2 = obs_hotkey_register_frontend("dust_manipulator_filter.hotkey2", obs_module_text("Hotkey 2"), handle_hotkey, NULL);
	s_Hotkey3 = obs_hotkey_register_frontend("dust_manipulator_filter.hotkey3", obs_module_text("Hotkey 3"), handle_hotkey, NULL);
	s_Hotkey4 = obs_hotkey_register_frontend("dust_manipulator_filter.hotkey4", obs_module_text("Hotkey 4"), handle_hotkey, NULL);
	s_HotkeysInitialized = true;

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	// Unload hotkeys
	if (s_HotkeysInitialized) 
	{
		obs_hotkey_unregister(s_Hotkey1);
		obs_hotkey_unregister(s_Hotkey2);
		obs_hotkey_unregister(s_Hotkey3);
		obs_hotkey_unregister(s_Hotkey4);
		s_HotkeysInitialized = false;
	}

	obs_log(LOG_INFO, "plugin unloaded");
}
