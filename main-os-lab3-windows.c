#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct
{
    uint64_t first_sample;
    uint64_t sample_count;
    uint64_t hits;
    uint64_t random_state;
    DWORD thread_id;
} WorkerData;

static uint64_t next_random(uint64_t *state)
{
    uint64_t value = *state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return value * UINT64_C(2685821657736338717);
}

static double random_unit(uint64_t *state)
{
    return (double)(next_random(state) >> 11) * (1.0 / 9007199254740992.0);
}

static DWORD WINAPI count_points(LPVOID argument)
{
    WorkerData *data = (WorkerData *)argument;
    uint64_t hits = 0;

    for (uint64_t i = 0; i < data->sample_count; ++i)
    {
        const double x = 2.0 * random_unit(&data->random_state) - 1.0;
        const double y = 2.0 * random_unit(&data->random_state) - 1.0;
        if (x * x + y * y <= 1.0)
        {
            ++hits;
        }
    }

    data->hits = hits;
    return 0;
}

static int parse_positive_double(const char *text, double *value)
{
    char *end = NULL;
    errno = 0;
    const double parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed) || parsed <= 0.0)
    {
        return 0;
    }
    *value = parsed;
    return 1;
}

static int parse_positive_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    errno = 0;
    if (text[0] == '-')
    {
        return 0;
    }
    const unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0)
    {
        return 0;
    }
    *value = (uint64_t)parsed;
    return 1;
}

static double milliseconds(LARGE_INTEGER start, LARGE_INTEGER finish, LARGE_INTEGER frequency)
{
    return 1000.0 * (double)(finish.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
}

int main(int argc, char **argv)
{
    double radius;
    uint64_t sample_total;
    uint64_t requested_threads;

    if (argc != 4 ||
        !parse_positive_double(argv[1], &radius) ||
        !parse_positive_u64(argv[2], &sample_total) ||
        !parse_positive_u64(argv[3], &requested_threads))
    {
        fprintf(stderr, "Usage: %s <radius> <number_of_points> <max_threads>\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (requested_threads > MAXIMUM_WAIT_OBJECTS || requested_threads > sample_total)
    {
        fprintf(stderr, "Thread count must be between 1 and min(number_of_points, %u).\n",
                (unsigned)MAXIMUM_WAIT_OBJECTS);
        return EXIT_FAILURE;
    }

    const DWORD thread_count = (DWORD)requested_threads;
    WorkerData *workers = (WorkerData *)calloc(thread_count, sizeof(*workers));
    HANDLE *handles = (HANDLE *)calloc(thread_count, sizeof(*handles));
    if (workers == NULL || handles == NULL)
    {
        fprintf(stderr, "Could not allocate memory for thread data.\n");
        free(workers);
        free(handles);
        return EXIT_FAILURE;
    }

    const uint64_t base_count = sample_total / thread_count;
    const uint64_t remainder = sample_total % thread_count;
    const DWORD process_id = GetCurrentProcessId();
    const DWORD main_thread_id = GetCurrentThreadId();
    LARGE_INTEGER frequency, start, finish;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&start))
    {
        fprintf(stderr, "QueryPerformanceCounter failed (error %lu).\n", GetLastError());
        free(workers);
        free(handles);
        return EXIT_FAILURE;
    }

    printf("PID=%lu, main TID=%lu\n", (unsigned long)process_id, (unsigned long)main_thread_id);
    printf("Requested worker threads: %lu\n", (unsigned long)thread_count);

    DWORD created = 0;
    for (DWORD i = 0; i < thread_count; ++i)
    {
        workers[i].first_sample = (uint64_t)i * base_count + (i < remainder ? i : remainder);
        workers[i].sample_count = base_count + (i < remainder ? 1 : 0);
        workers[i].random_state = UINT64_C(0x9E3779B97F4A7C15) ^
                                  ((uint64_t)process_id << 32) ^
                                  ((uint64_t)(i + 1) * UINT64_C(0xD1B54A32D192ED03));
        if (workers[i].random_state == 0)
        {
            workers[i].random_state = (uint64_t)i + 1;
        }

        handles[i] = CreateThread(NULL, 0, count_points, &workers[i], 0, &workers[i].thread_id);
        if (handles[i] == NULL)
        {
            fprintf(stderr, "CreateThread failed (error %lu).\n", GetLastError());
            break;
        }
        ++created;
        printf("Worker %lu: TID=%lu, points=%" PRIu64 "\n",
               (unsigned long)i, (unsigned long)workers[i].thread_id, workers[i].sample_count);
    }

    if (created != thread_count)
    {
        if (created > 0)
        {
            WaitForMultipleObjects(created, handles, TRUE, INFINITE);
        }
        for (DWORD i = 0; i < created; ++i)
        {
            CloseHandle(handles[i]);
        }
        free(workers);
        free(handles);
        return EXIT_FAILURE;
    }

    const DWORD wait_result = WaitForMultipleObjects(thread_count, handles, TRUE, INFINITE);
    if (wait_result == WAIT_FAILED)
    {
        fprintf(stderr, "WaitForMultipleObjects failed (error %lu).\n", GetLastError());
        for (DWORD i = 0; i < thread_count; ++i)
        {
            CloseHandle(handles[i]);
        }
        free(workers);
        free(handles);
        return EXIT_FAILURE;
    }
    if (!QueryPerformanceCounter(&finish))
    {
        fprintf(stderr, "QueryPerformanceCounter failed (error %lu).\n", GetLastError());
        for (DWORD i = 0; i < thread_count; ++i)
        {
            CloseHandle(handles[i]);
        }
        free(workers);
        free(handles);
        return EXIT_FAILURE;
    }

    uint64_t total_hits = 0;
    for (DWORD i = 0; i < thread_count; ++i)
    {
        total_hits += workers[i].hits;
        CloseHandle(handles[i]);
    }

    const double area = 4.0 * radius * radius * (double)total_hits / (double)sample_total;
    const double elapsed_ms = milliseconds(start, finish, frequency);
    printf("Points: %" PRIu64 ", inside circle: %" PRIu64 "\n", sample_total, total_hits);
    printf("Estimated circle area: %.10f\n", area);
    printf("Elapsed time: %.3f ms\n", elapsed_ms);

    free(workers);
    free(handles);
    return EXIT_SUCCESS;
}
