#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "scanivalve/mps-protocol.h"

#define MAX_TRACKED_FRAME_GAPS 1024

typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float    f32;
typedef double   f64;


typedef struct {
    u8* data;
    u64 size;
} ByteArray;

typedef struct {
    u8 type;
    u8 size;
} Packet;

typedef struct {
    char filepath[64];
    u32  device_type;
    u64  file_size;
    u8   packet_type;
    u16  packet_size;
    u8   units_index;
    u8   temperature_channel_count;
    u8   pressure_channel_count;
    char start_time[32];
    f32  duration;
    f32  rate;
    u64  count;
    u64  missing;
    u64  repeated;
    u32  missing_frames[MAX_TRACKED_FRAME_GAPS];
    u32  missing_frames_len;
    u32  repeate_frames[MAX_TRACKED_FRAME_GAPS];
    u32  repeate_frames_len;
} Summary;



ByteArray read_file(const char* fname);
void destroy_bytes(ByteArray* bytes);
void dump_bytes(FILE* f, ByteArray* bytes);

int summarize(ByteArray* bytes, Summary* sum);
void print_summary(FILE* f, Summary* sum);


int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "Not enough arguments, filepath required\n");
        return 1;
    }
    Summary summary =  {0};

    const char* fname = argv[1];
    snprintf(summary.filepath, sizeof(summary.filepath),"%s", fname);

    ByteArray bytes = read_file(fname);

    //- determine the packet type (assume constant for whole file)
    u8 packet_type = bytes.data[0];
    const MpsBinaryPacketInfo* info = mps_get_binary_packet_info_by_type(packet_type);
    if (!info)
    {
        fprintf(stderr, "Error: Unknown Packet Type `0x%02x`", packet_type);
        destroy_bytes(&bytes);
        return 1;
    }

    if (!summarize(&bytes, &summary))
    {
        fprintf(stderr, "Failed to summarize file contents.\n");
        destroy_bytes(&bytes);
        return 1;
    }

    print_summary(stdout, &summary);
    destroy_bytes(&bytes);

    return 0;
}

ByteArray read_file(const char* fname)
{
    FILE* f = fopen(fname, "rb");
    if (!f) return (ByteArray){0};

    fseek(f, 0, SEEK_END);
    u64 size = (u64)ftell(f);
    fseek(f, 0, SEEK_SET);

    ByteArray out = {
        .data = (u8*)malloc(size),
        .size = size
    };

    if (!out.data)
    {
        fclose(f);
        return (ByteArray){0};
    }

    size_t n = fread(out.data, 1, (size_t)out.size, f);
    if (n != out.size)
    {
        fprintf(stderr, "Short read: expected %llu bytes, got %zu bytes\n", out.size, n);
    }

    fclose(f);

    return out;
}

void destroy_bytes(ByteArray* bytes)
{
    if (!bytes) return;
    if (bytes->data == NULL)
    {
        bytes->size = 0;
        return;
    }

    free(bytes->data);
    bytes->data = NULL;
    bytes->size = 0;
}

void dump_bytes(FILE* f, ByteArray* bytes)
{
    for (u64 i = 0; i < bytes->size; ++i)
    {
        fprintf(f, "%02X", bytes->data[i]);
        if ((i+1) % 4 == 0) fprintf(f, "  ");
        else fprintf(f, " ");

        if ((i+1) % 16 == 0) fprintf(f, "\n");
    }
}

int summarize(ByteArray* bytes, Summary* sum)
{
    if (!bytes || !sum || !bytes->data)
        return 0;

    if (bytes->size < sizeof(i32))
    {
        fprintf(stderr, "File too small to contain packet header\n");
        return 0;
    }

    /*
     * Assume little-endian layout on disk, matching the in-memory structs
     * from mps-protocol.h. The first 4 bytes are an int32_t type field, and
     * the low byte holds the packet type ID (e.g. 0x0A, 0x5D, 0x5B, ...).
     */
    u8 type_id = bytes->data[0];

    const MpsBinaryPacketInfo* info = mps_get_binary_packet_info_by_type(type_id);
    if (!info)
    {
        fprintf(stderr, "Unknown packet type: 0x%02X\n", type_id);
        return 0;
    }

    sum->file_size   = bytes->size;
    sum->packet_type = type_id;
    sum->packet_size = info->size_bytes;
    sum->count       = 0;
    sum->missing     = 0;
    sum->duration    = 0.0f;
    sum->rate        = 0.0f;
    sum->units_index = 0;
    sum->device_type = 0; /* unknown here; caller can fill if known */
    sum->temperature_channel_count = info->temperature_channel_count;
    sum->pressure_channel_count = info->pressure_channel_count;
    sum->start_time[0] = '\0';

    if (bytes->size % info->size_bytes != 0)
        fprintf(stderr, "Warning: file size (%llu) is not a multiple of packet size (%u)\n", bytes->size, info->size_bytes);

    u64 packet_count = bytes->size / info->size_bytes;

    if (packet_count == 0) return 1; /* nothing to do, but summary is valid */

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
        const u8* base = bytes->data + i * info->size_bytes;

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
                    sum->missing++;
                    if (sum->missing_frames_len < MAX_TRACKED_FRAME_GAPS)
                        sum->missing_frames[sum->missing_frames_len++] = m;
                }
            }
            else if (frame == last_frame)
            {
                sum->repeated++;
                if (sum->repeate_frames_len < MAX_TRACKED_FRAME_GAPS)
                    sum->repeate_frames[sum->repeate_frames_len++] = frame;
            }
            else 
            {
                // now frame must be < last frame (i.e., out of order)
                sum->repeated++;
                if (sum->repeate_frames_len < MAX_TRACKED_FRAME_GAPS)
                    sum->repeate_frames[sum->repeate_frames_len++] = frame;
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
        sum->count++;
    }

    /* Derive duration and sampling rate from timestamps if available. */
    if (have_time && sum->count > 1)
    {
        f64 dt = last_time - first_time;
        if (dt > 0.0)
        {
            sum->duration = (f32)dt + frame_duration;

            /* If the packet itself did not provide a framerate, estimate it. */
            if (sum->rate == 0.0f)
            {
                sum->rate = (f32)((f64)(sum->count - 1) / dt);
            }
        }
    }

    // - generate start scan time string
    struct timespec ts;
    ts.tv_sec = start_time_s;
    ts.tv_nsec = start_time_ns;

    struct tm* tm_info;
    tm_info = localtime(&ts.tv_sec);
    strftime(sum->start_time, sizeof(sum->start_time), "%d-%m-%Y %H:%M:%S", tm_info);
    snprintf(sum->start_time, sizeof(sum->start_time), "%s.%09ld", sum->start_time, ts.tv_nsec);


    return 1;
}


void print_summary(FILE* f, Summary* sum)
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
    fprintf(f, " -- Total Frames    %39llu\n", sum->count);
    fprintf(f, " -- Missing Frames  %39llu\n", sum->missing);
    fprintf(f, " -- Repeated Frames %39llu\n", sum->repeated);
    fprintf(f, "-----------------------------------------------------------\n");

    if (sum->missing)
    {
        fprintf(f, " -- Missing Frames -- \n");
        for (u32 i = 0; i < sum->missing_frames_len; ++i)
        {
            fprintf(f, "%5u ", sum->missing_frames[i]);
            if ((i+1) % 10 == 0) fprintf(f, "\n");
        }
        fprintf(f, "\n");
    }
    if (sum->repeated)
    {
        fprintf(f, " -- Repeated Frames -- \n");
        for (u32 i = 0; i < sum->repeate_frames_len; ++i)
        {
            fprintf(f, "%5u ", sum->repeate_frames[i]);
            if ((i+1) % 10 == 0) fprintf(f, "\n");
        }
        fprintf(f, "\n");
    }

}
