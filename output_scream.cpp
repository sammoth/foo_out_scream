#include "stdafx.h"
#include "SDK/output.h"
#include "SDK/audio_chunk_impl.h"
#include <winsock2.h> // before Windows.h, else Winsock 1 conflict
#include <ws2tcpip.h> // needed for ip_mreq definition for multicast
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ks.h>
#include <ksmedia.h>
#include <mutex>
#include <Mmsystem.h>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Winmm.lib")

#define TIMER_INTERVAL_MS 10
#define TIMER_TARGET_RESOLUTION_MS 1
#define BUFFER_BEFORE_PLAYBACK_MS 300
#define SEND_AHEAD_MS 20

static const GUID guid_scream_branch = { 0x18564ced, 0x4abf, 0x4f0c, { 0xa4, 0x43, 0x98, 0xda, 0x88, 0xe2, 0xcd, 0x39 } };
static const GUID guid_cfg_destination_port = { 0xe7598963, 0xed60, 0x4084, { 0xa8, 0x5d, 0xd1, 0xcd, 0xc5, 0x51, 0x22, 0xca } };

static advconfig_branch_factory g_scream_output_branch("Scream output", guid_scream_branch, advconfig_branch::guid_branch_playback, 0);
static advconfig_integer_factory cfg_scream_destination_port("Destination port", guid_cfg_destination_port, guid_scream_branch, 0, 4010, 0, 65535, 0);

namespace {
	class scream_player {
	public:
		const t_samplespec spec;

		scream_player() :
			max_size_(),
			buf_(),
			bitdepth(),
			spec(),
			m_bChannels(),
			m_wChannelMask(),
			m_bBitsPerSampleMarker(),
			m_bSamplingFreqMarker(),
			send_ahead_frames(),
			timer_id(NULL),
			timer_resolution(NULL)
		{}


		scream_player(double buffer_length_seconds, t_samplespec p_spec, size_t p_bitdepth, bool p_dither, int port) :
			max_size_(buffer_length_seconds* p_bitdepth* p_spec.m_channels* p_spec.m_sample_rate / 8),
			buf_(std::unique_ptr<BYTE>(new BYTE[buffer_length_seconds * p_bitdepth * p_spec.m_channels * p_spec.m_sample_rate / 8])),
			bitdepth(p_bitdepth),
			dither(p_dither),
			spec(p_spec),
			m_bChannels((BYTE)spec.m_channels),
			m_bBitsPerSampleMarker((BYTE)p_bitdepth),
			postprocessor(standard_api_create_t<audio_postprocessor>()),
			m_bSamplingFreqMarker((BYTE)((spec.m_sample_rate % 44100) ? (0 + (spec.m_sample_rate / 48000)) : (128 + (spec.m_sample_rate / 44100)))),
			waiting(true),
			send_ahead_frames(p_spec.time_to_samples(0.001 * SEND_AHEAD_MS)* p_spec.m_channels* p_bitdepth / 9216)
		{
			switch (spec.m_channel_config)
			{
			case audio_chunk::channel_config_mono: m_wChannelMask = (WORD)KSAUDIO_SPEAKER_MONO; break;
			case audio_chunk::channel_config_stereo: m_wChannelMask = (WORD)KSAUDIO_SPEAKER_STEREO; break;
			case audio_chunk::channel_config_4point0: m_wChannelMask = (WORD)KSAUDIO_SPEAKER_QUAD; break;
			case audio_chunk::channel_config_5point0: m_wChannelMask = (WORD)KSAUDIO_SPEAKER_5POINT0; break;
			case audio_chunk::channel_config_5point1: m_wChannelMask = (WORD)KSAUDIO_SPEAKER_5POINT1; break;
			case audio_chunk::channel_config_7point1: m_wChannelMask = (WORD)KSAUDIO_SPEAKER_7POINT1; break;
			case audio_chunk::channel_config_5point1_side: m_wChannelMask = (WORD)KSAUDIO_SPEAKER_5POINT1; break;
			default: m_wChannelMask = (WORD)KSAUDIO_SPEAKER_STEREO;
			}

			const char* multicast_group = "239.255.77.77";
			WSADATA wsaData;
			if (WSAStartup(0x0101, &wsaData)) {
				perror("WSAStartup");
			}
			udp_socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
			if (udp_socket_fd < 0) {
				perror("socket");
			}
			memset(&udp_addr, 0, sizeof(udp_addr));
			udp_addr.sin_family = AF_INET;
			inet_pton(AF_INET, multicast_group, &(udp_addr.sin_addr.s_addr));
			udp_addr.sin_port = htons(port);

			TIMECAPS tc;
			timeGetDevCaps(&tc, sizeof(TIMECAPS));
			timer_resolution = max(min(max(tc.wPeriodMin, 0), tc.wPeriodMax), TIMER_TARGET_RESOLUTION_MS);
			timeBeginPeriod(timer_resolution);
			timer_id = timeSetEvent(
				TIMER_INTERVAL_MS,
				timer_resolution,
				TimerCallback,
				(DWORD)this,
				TIME_PERIODIC || TIME_KILL_SYNCHRONOUS);

			output_started = std::chrono::high_resolution_clock::now();
			frames_sent = 0;
		};
		~scream_player()
		{
			std::lock_guard<std::mutex> lock(buffer_mutex_);
			if (timer_id != NULL)
			{
				timeKillEvent(timer_id);
				timer_id = NULL;

				for (int i = 0; i < 20; i++)
				{
					BYTE frame[1157] = {};

					frame[0] = m_bSamplingFreqMarker;
					frame[1] = m_bBitsPerSampleMarker;
					frame[2] = m_bChannels;
					frame[3] = (BYTE)(m_wChannelMask & 0xFF);
					frame[4] = (BYTE)(m_wChannelMask >> 8 & 0xFF);
					send_udp(frame);
				}
			}
			if (timer_resolution != NULL)
			{
				timeEndPeriod(timer_resolution);
				timer_resolution = NULL;
			}
		}

		void play() {
			playing = true;
		}
		void pause() {
			playing = false;
		}
		void force_play() {
			force_play_flag = true;
		}
		bool is_progressing() {
			return !waiting;
		}
		void queue(audio_chunk_impl chunk)
		{
			postprocessor->run(chunk, incoming, bitdepth, bitdepth, dither, 1.0f);
			{
				std::lock_guard<std::mutex> lock(buffer_mutex_);
				size_t copy_amount = pfc::min_t(space(), incoming.get_size());
				size_t endspace = max_size_ - head_;
				memcpy(buf_.get() + head_, incoming.get_ptr(), pfc::min_t(copy_amount, endspace));
				if (endspace < copy_amount)
				{
					memcpy(buf_.get(), ((BYTE*)incoming.get_ptr()) + endspace, incoming.get_size() - endspace);
				}
				head_ = (head_ + copy_amount) % max_size_;
				full_ = head_ == tail_;
			}

			if (waiting && size() > pfc::min_t(max_size_ / 2, bitdepth * spec.time_to_samples(BUFFER_BEFORE_PLAYBACK_MS * 0.001) * spec.m_channels / 8))
			{
				waiting = false;
			}
		}
		void reset()
		{
			std::lock_guard<std::mutex> lock(buffer_mutex_);
			head_ = tail_;
			full_ = false;
			waiting = true;
		}
		bool empty() const
		{
			return (!full_ && (head_ == tail_));
		}
		bool full() const
		{
			return full_;
		}

		size_t can_write_samples()
		{
			return space() * 8 / bitdepth;
		}

		size_t samples_queued()
		{
			return size() * 8 / bitdepth;
		}

	private:
		std::mutex buffer_mutex_;
		std::unique_ptr<BYTE> buf_;
		size_t head_ = 0;
		size_t tail_ = 0;
		size_t max_size_;
		bool full_ = 0;

		struct sockaddr_in udp_addr;
		int udp_socket_fd;

		UINT timer_resolution;
		MMRESULT timer_id = NULL;
		std::chrono::time_point<std::chrono::high_resolution_clock> output_started;
		t_uint32 frames_sent;
		const t_uint32 send_ahead_frames;

		size_t bitdepth;
		bool dither;
		const BYTE m_bChannels;
		WORD m_wChannelMask;
		const BYTE m_bSamplingFreqMarker;
		const BYTE m_bBitsPerSampleMarker;

		service_ptr_t<audio_postprocessor> postprocessor;
		mem_block_container_impl incoming;

		bool playing = false;
		bool force_play_flag = false;
		bool waiting = true;

		size_t capacity() const
		{
			return max_size_;
		}
		size_t size() const
		{
			size_t size = max_size_;
			if (!full_)
			{
				if (head_ >= tail_)
				{
					size = head_ - tail_;
				}
				else
				{
					size = max_size_ + head_ - tail_;
				}
			}
			return size;
		}
		size_t space() const
		{
			return max_size_ - size();
		}

		static void CALLBACK TimerCallback(UINT wTimerID, UINT msg,
			DWORD dwUser, DWORD dw1, DWORD dw2)
		{
			scream_player* obj = (scream_player*)dwUser;
			std::lock_guard<std::mutex> lock(obj->buffer_mutex_);
			obj->send_frames();
		}

		void send_frames()
		{
			auto now = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double> elapsed_seconds = now - output_started;
			size_t frames_target = ceil((double)spec.time_to_samples(elapsed_seconds.count()) * spec.m_channels * bitdepth / 9216.0) + send_ahead_frames - frames_sent;

			for (int i = 0; i < frames_target; i++) {
				BYTE frame[1157] = {};
				BYTE m_bChannels = (BYTE)spec.m_channels;
				frame[0] = m_bSamplingFreqMarker;
				frame[1] = m_bBitsPerSampleMarker;
				frame[2] = m_bChannels;
				frame[3] = (BYTE)(m_wChannelMask & 0xFF);
				frame[4] = (BYTE)(m_wChannelMask >> 8 & 0xFF);

				if (playing && (force_play_flag || !waiting))
				{
					size_t take = min(1152, size());
					if (take == size())
						force_play_flag = false;

					size_t endspace = max_size_ - tail_;
					memcpy(frame + 5, buf_.get() + tail_, min(endspace, take));
					if (endspace < take)
					{
						memcpy(frame + 5 + endspace, buf_.get(), take - endspace);
					}

					full_ = false;
					tail_ = (tail_ + take) % max_size_;
				}


				send_udp(frame);
			}

			frames_sent += frames_target;
			if (frames_sent > 100000)
			{
				frames_sent = send_ahead_frames;
				output_started = now;
			}
		}

		void send_udp(BYTE frame[]) {
			char ch = 0;
			int nbytes = sendto(
				udp_socket_fd,
				(char*)frame,
				1157,
				0,
				(struct sockaddr*)&udp_addr,
				sizeof(udp_addr)
			);
			if (nbytes < 0) {
				console::error("Scream: error sending UDP frame");
			}
		}
	};

	class output_scream : public output_impl
	{
	private:
		pfc::array_t<audio_sample, pfc::alloc_fast_aggressive> m_incoming;
		t_size m_incoming_ptr;
		t_samplespec m_incoming_spec;
		size_t bitdepth;
		bool dither;
		double buffer_length_seconds;

		std::unique_ptr<scream_player> player;
	public:
		output_scream(const GUID& p_device, double p_buffer_length, bool p_dither, t_uint32 p_bitdepth)
			: m_incoming_ptr(0), buffer_length_seconds(p_buffer_length), bitdepth(p_bitdepth), dither(p_dither),
			player(std::unique_ptr<scream_player>(new scream_player()))
		{
		}

		void on_update() override
		{
		}
		void write(const audio_chunk& p_data) override
		{
			player->queue(p_data);
		}
		t_size can_write_samples() override
		{
			if (player->spec.is_valid())
			{
				return player->can_write_samples() / player->spec.m_channels;
			}
			else
			{
				return 0;
			}
		}
		t_size get_latency_samples() override
		{
			if (player->spec.m_channels > 0)
			{
				return player->samples_queued() / player->spec.m_channels;
			}
			else
			{
				return 0;
			}
		}
		void on_flush() override
		{
			player->reset();
		}
		void on_flush_changing_track() override
		{
			player->reset();
		}
		void open(t_samplespec const& p_spec) override
		{
			if (p_spec.m_sample_rate % 44100 != 0 && p_spec.m_sample_rate % 48000 != 0)
			{
				pfc::throw_exception_with_message< exception_io_data >("Invalid sample rate for Scream playback - must be multiple of 44100Hz or 48000Hz");
			}

			player = std::unique_ptr<scream_player>(new scream_player(buffer_length_seconds, p_spec, bitdepth, dither, cfg_scream_destination_port.get()));
			player->play();
		}
		void pause(bool p_state) override
		{
			if (p_state)
			{
				player->pause();
			}
			else
			{
				player->play();
			}
		}
		void force_play() override
		{
			player->force_play();
		}
		void volume_set(double p_val) override
		{
		}
		bool is_progressing() override
		{
			return player->is_progressing();
		}
		static void g_enum_devices(output_device_enum_callback& p_callback) {
			const GUID device = { 0x290538b, 0x27da, 0x4ca7, { 0x4a, 0x1b, 0xaf, 0x91, 0x8b, 0xa, 0x15, 0xd0 } };
			p_callback.on_device(device, "Multicast", 9);
		}
		static GUID g_get_guid() {
			//This is our GUID. Generate your own one when reusing this code.
			static const GUID guid = { 0x410960c, 0x14bf, 0x3491, { 0xf8, 0x19, 0x50, 0x6a, 0x9b, 0x63, 0x48, 0x82 } };
			return guid;
		}
		static bool g_advanced_settings_query() { return false; }
		static bool g_needs_bitdepth_config() { return true; }
		static bool g_needs_dither_config() { return true; }
		static bool g_needs_device_list_prefixes() { return false; }
		static bool g_supports_multiple_streams() { return false; }
		static bool g_is_high_latency() { return false; }
		static uint32_t g_extra_flags() {
			return 0;
		}
		static void g_advanced_settings_popup(HWND p_parent, POINT p_menupoint) {}
		static const char* g_get_name() { return "Scream"; }
	};

	static output_factory_t<output_scream> g_output_sample_factory;
}