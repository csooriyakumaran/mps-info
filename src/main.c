#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "version.h"

#include "scanivalve/mps-protocol.h"
#include "scanivalve/mps-protocol-version.h"

#define AETHER_IMPLEMENTATION
#include "aether/aether.h"

#define MAX_TRACKED_FRAME_GAPS 256

typedef struct {
    char* filepath;
    u64  file_size;

    u64  captured_frame_count;
    u64  missing_frame_count;
    u64  repeated_frame_count;

    u32  device_type;
    u16  packet_size;
    u8   packet_type;
    u8   units_index;
    u8   temperature_channel_count;
    u8   pressure_channel_count;

    f32  duration;
    f32  rate;

    u32* missing_frames;
    u32  missing_frames_len;
    u32* repeat_frames;
    u32  repeat_frames_len;
    char start_time[32];
} Summary;

int evaluate_packets(str8 bytes, Summary* sum, Arena* arena);
void print_summary(FILE* f, const Summary* sum);

int main(int argc, char** argv)
{
    const char* fname = NULL;
    b8 mapfile        = false;

    if (argc < 2)
    {
        fprintf(stderr, "Not enough arguments, filepath required\n");
        return 1;
    }

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--version") == 0|| strcmp(argv[i], "-v") == 0 )
        {
            fprintf(stdout, "v%s\n", MPS_INFO_VERSION_STRING);
            fprintf(stdout, "  - Scanivalve Protocol Version v%s\n  - Scanivalve Firmware Version v%s\n", MPS_PROTOCOL_VERSION_STRING, MPS_FIRMWARE_VERSION_STRING );
            fprintf(stdout, "  -              Aether Verison v%s\n", AETHER_VERSION_STRING);
            return 0;
        } else if (strcmp(argv[i], "--map-file") == 0) {
            mapfile = true;
        } else if (!fname) {
            fname = argv[i];
        }
    }

    Arena   arena   = arena_alloc(GB(2));
    Summary summary = {0};
    summary.filepath = arena_push_cstring(&arena, fname);

    str8 bytes = {0};
    u64 start = time_mark();

    if (mapfile)
        bytes = map_file(summary.filepath);
    else
        bytes = arena_read_file(&arena, summary.filepath);

    if (!bytes.data || bytes.size == 0)
    {
        fprintf(stderr, "Error: No data read or mapped from %s", summary.filepath);
        arena_release(&arena);
        return 1;
    }

    u64 file_read_end = time_mark();

    //- determine the packet type (assume constant for whole file)
    u8 packet_type = bytes.data[0];
    const MpsBinaryPacketInfo* info = mps_get_binary_packet_info_by_type(packet_type);
    if (!info)
    {
        fprintf(stderr, "Error: Unknown Packet Type `0x%02x`", packet_type);
        arena_release(&arena);
        return 1;
    }

    if (!evaluate_packets(bytes, &summary, &arena))
    {
        fprintf(stderr, "Failed to summarize file contents.\n");
        arena_release(&arena);
        return 1;
    }

    u64 end = time_mark();
    print_summary(stdout, &summary);

    f64 file_read_time  = time_elapsed_sec(start, file_read_end) * 1000.0;
    f64 processing_time = time_elapsed_sec(file_read_end, end) * 1000.0;
    f64 elapsed_time    = time_elapsed_sec(start, end) * 1000.0;
    fprintf(stdout, " -- File %-6s       %34.2f ms\n", mapfile ? "Mapped" : "Read", file_read_time);
    fprintf(stdout, " -- Data Processed  %36.2f ms\n", processing_time);
    fprintf(stdout, "-----------------------------------------------------------\n");
    fprintf(stdout, " -- Total Time      %36.2f ms\n", elapsed_time);

    if (mapfile) unmap_file(bytes);
    arena_release(&arena);
    return 0;
}

int evaluate_packets(str8 bytes, Summary* sum, Arena* arena)
{
    if (!sum || !bytes.data)
        return 0;

    if (bytes.size < sizeof(i32))
    {
        fprintf(stderr, "File too small to contain packet header\n");
        return 0;
    }

    /*
     * Assume little-endian layout on disk, matching the in-memory structs
     * from mps-protocol.h. The first 4 bytes are an int32_t type field, and
     * the low byte holds the packet type ID (e.g. 0x0A, 0x5D, 0x5B, ...).
     */
    u8 type_id = bytes.data[0];

    const MpsBinaryPacketInfo* info = mps_get_binary_packet_info_by_type(type_id);
    if (!info)
    {
        fprintf(stderr, "Unknown packet type: 0x%02X\n", type_id);
        return 0;
    }

    if (bytes.size % info->size_bytes != 0)
        fprintf(stderr, "Warning: file size (%llu) is not a multiple of packet size (%u)\n", bytes.size, info->size_bytes);

    u64 packet_count = bytes.size / info->size_bytes;

    if (packet_count == 0) return 1; /* nothing to do, but summary is valid */

    sum->file_size            = bytes.size;
    sum->packet_type          = type_id;
    sum->packet_size          = info->size_bytes;
    sum->captured_frame_count = 0;
    sum->missing_frame_count  = 0;
    sum->repeated_frame_count = 0;
    sum->duration             = 0.0f;
    sum->rate                 = 0.0f;
    sum->units_index          = 0;
    sum->device_type          = 0; /* unknown here; caller can fill if known */
    sum->temperature_channel_count = info->temperature_channel_count;
    sum->pressure_channel_count    = info->pressure_channel_count;
    sum->start_time[0]  = '\0';
    sum->missing_frames = arena_push_array(arena, u32, MAX_TRACKED_FRAME_GAPS);
    sum->repeat_frames  = arena_push_array(arena, u32, MAX_TRACKED_FRAME_GAPS);

    u32 frame          = 0;
    u32 first_frame    = 0;
    u32 last_frame     = 0;
    u32 start_time_s   = 0;
    u32 start_time_ns  = 0;
    f64 first_time     = 0.0;
    f64 last_time      = 0.0;
    int have_time      = 0;
    f64 frame_duration = 0.0;
    f64 frame_time     = 0.0;

    for (u64 i = 0; i < packet_count; ++i)
    {
        const u8* base = bytes.data + i * info->size_bytes;

        switch (info->type)
        {
            case MPS_PKT_LEGACY_TYPE:
            {
                sum->device_type = MPS_4264;
                const MpsLegacyPacket* pkt = (const MpsLegacyPacket*)base;

                frame      = (u32)pkt->frame;
                frame_time = (f64)pkt->frame_time_s + (f64)pkt->frame_time_ns * 1e-9;

                if (i == 0)
                {
                    sum->units_index = (u8)pkt->unit_index;
                    sum->rate        = pkt->framerate; /* already Hz */
                    start_time_s  = pkt->ptp_scan_start_time_s;
                    start_time_ns = pkt->ptp_scan_start_time_ns;
                }

                break;
            }

            case MPS_PKT_16_TYPE:
            case MPS_PKT_16_RAW_TYPE:
            {
                sum->device_type = MPS_4216;
                const Mps16Packet* pkt = (const Mps16Packet*)base;
                frame      = pkt->frame;
                frame_time = (f64)pkt->frame_time_s + (f64)pkt->frame_time_ns * 1e-9;

                break;
            }
            case MPS_PKT_32_TYPE:
            case MPS_PKT_32_RAW_TYPE:
            {
                sum->device_type = MPS_4232;
                const Mps32Packet* pkt = (const Mps32Packet*)base;
                frame      = pkt->frame;
                frame_time = (f64)pkt->frame_time_s + (f64)pkt->frame_time_ns * 1e-9;

                break;
            }
            case MPS_PKT_64_TYPE:
            case MPS_PKT_64_RAW_TYPE:
            {
                sum->device_type = MPS_4264;
                const Mps64Packet* pkt = (const Mps64Packet*)base;
                frame      = pkt->frame;
                frame_time = (f64)pkt->frame_time_s + (f64)pkt->frame_time_ns * 1e-9;

                break;
            }

            default:
                /* For other packet types, you can follow the same pattern. */
                break;
        }

        // - handle missing / repeated frames
        if (i == 0)
        {
            first_time = frame;
            last_frame = frame;
        }
        else
        {
            if (frame == last_frame + 1)
            { /* GOOD - no-op */ }
            else if (frame > last_frame + 1) 
            {
                for (u32 m = last_frame + 1; m < frame; ++m)
                {
                    sum->missing_frame_count++;
                    if (sum->missing_frames_len < MAX_TRACKED_FRAME_GAPS)
                        sum->missing_frames[sum->missing_frames_len++] = m;
                }
            }
            else if (frame == last_frame)
            {
                sum->repeated_frame_count++;
                if (sum->repeat_frames_len < MAX_TRACKED_FRAME_GAPS)
                    sum->repeat_frames[sum->repeat_frames_len++] = frame;
            }
            else 
            {
                // now frame must be < last frame (i.e., out of order)
                sum->repeated_frame_count++;
                if (sum->repeat_frames_len < MAX_TRACKED_FRAME_GAPS)
                    sum->repeat_frames[sum->repeat_frames_len++] = frame;
            }

        }
        if (frame > last_frame)
            last_frame = frame;

        if (!have_time)
        {
            first_time = frame_time;
            last_time  = frame_time;
            have_time  = 1;
        }
        else if (frame_time > last_time)
        {
            last_time = frame_time;
        }

        if (i == 1)
        {
            if (have_time) frame_duration = last_time - first_time;
        }
        sum->captured_frame_count++;
    }

    /* Derive duration and sampling rate from timestamps if available. */
    if (have_time && sum->captured_frame_count > 1)
    {
        f64 dt = last_time - first_time;
        if (dt > 0.0)
        {
            sum->duration = (f32)dt + frame_duration;

            /* If the packet itself did not provide a framerate, estimate it. */
            if (sum->rate == 0.0f)
            {
                sum->rate = (f32)((f64)(sum->captured_frame_count - 1) / dt);
            }
        }
    }

    // - generate start scan time string
    struct timespec ts;
    ts.tv_sec = start_time_s;
    ts.tv_nsec = start_time_ns;

    struct tm* tm_info;
    tm_info = localtime(&ts.tv_sec);
    char date_buf[20];
    strftime(date_buf, sizeof(date_buf), "%d-%m-%Y %H:%M:%S", tm_info);
    snprintf(sum->start_time, sizeof(sum->start_time), "%s.%09ld", date_buf, ts.tv_nsec);

    return 1;
}


void print_summary(FILE* f, const Summary* sum)
{
    if (!sum) return;

    /*
     * Right-align all values after the colon in a fixed-width field so
     * the numeric/text columns line up nicely.
     */
    const int width = 40; /* adjust as desired for your layout */

    fprintf(f, "-----------------------------------------------------------\n");
    fprintf(f, " -- File Path --\n %58s\n", sum->filepath);
    fprintf(f, "-----------------------------------------------------------\n");
    fprintf(f, " -- Scanner Type    %*sMPS-%04u\n", width - 9, "", sum->device_type);
    fprintf(f, " -- File Size       %*s%10llu bytes\n", width - 17, "", sum->file_size);
    fprintf(f, " -- Packet Type     %*s0x%02X\n", width - 5, "", sum->packet_type);
    fprintf(f, " -- Start Time      %39s\n", sum->start_time[0] ? sum->start_time : "");
    fprintf(f, " -- Scan Duration   %36.3f  s\n", sum->duration);
    fprintf(f, " -- Scan Rate       %36.3f Hz\n", sum->rate);
    fprintf(f, " -- Units           %39s\n", mps_units_to_string(sum->units_index));
    fprintf(f, " -- Unit Conversion %39.3f\n", mps_units_conversion_factor(sum->units_index));
    fprintf(f, " -- T-Channels      %39u\n", sum->temperature_channel_count);
    fprintf(f, " -- P-Channels      %39u\n", sum->pressure_channel_count);
    fprintf(f, "-----------------------------------------------------------\n");
    fprintf(f, " -- Total Frames    %39llu\n", sum->captured_frame_count);
    fprintf(f, " -- Missing Frames  %39llu\n", sum->missing_frame_count);
    fprintf(f, " -- Repeated Frames %39llu\n", sum->repeated_frame_count);
    fprintf(f, "-----------------------------------------------------------\n");

    if (sum->missing_frame_count)
    {
        fprintf(f, " -- Missing Frames -- %s\n", sum->missing_frame_count > sum->missing_frames_len ? "(truncated)" : "");
        for (u32 i = 0; i < sum->missing_frames_len; ++i)
        {
            fprintf(f, "%5u ", sum->missing_frames[i]);
            if ((i+1) % 10 == 0) fprintf(f, "\n");
        }
        fprintf(f, "%s\n", sum->missing_frame_count > sum->missing_frames_len ? " ... " : "");
        fprintf(f, "-----------------------------------------------------------\n");
    }
    if (sum->repeated_frame_count)
    {
        fprintf(f, " -- Repeated Frames -- %s\n", sum->repeated_frame_count > sum->repeat_frames_len ? "(truncated)" : "");
        for (u32 i = 0; i < sum->repeat_frames_len; ++i)
        {
            fprintf(f, "%5u ", sum->repeat_frames[i]);
            if ((i+1) % 10 == 0) fprintf(f, "\n");
        }
        fprintf(f, "%s\n", sum->repeated_frame_count > sum->repeat_frames_len ? " ... " : "");
        fprintf(f, "-----------------------------------------------------------\n");
    }

}
