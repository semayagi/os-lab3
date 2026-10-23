#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
    uint64_t first_sample;
    uint64_t sample_count;
    uint64_t hits;
    uint64_t random_state;
    pthread_t thread_id;
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

static void *count_points(void *argument)
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
    return NULL;
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

static double elapsed_milliseconds(const struct timespec start, const struct timespec finish)
{
    long seconds = finish.tv_sec - start.tv_sec;
    long nanoseconds = finish.tv_nsec - start.tv_nsec;

    if (nanoseconds < 0)
    {
        --seconds;
        nanoseconds += 1000000000L;
    }

    return (double)seconds * 1000.0 + (double)nanoseconds / 1000000.0;
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

    if (requested_threads > sample_total)
    {
        fprintf(stderr, "Thread count must be between 1 and number_of_points.\n");
        return EXIT_FAILURE;
    }

    const size_t thread_count = (size_t)requested_threads;
    WorkerData *workers = calloc(thread_count, sizeof(*workers));
    if (workers == NULL)
    {
        fprintf(stderr, "Could not allocate memory for thread data.\n");
        return EXIT_FAILURE;
    }

    const uint64_t base_count = sample_total / thread_count;
    const uint64_t remainder = sample_total % thread_count;
    const pid_t process_id = getpid();
    const pthread_t main_thread_id = pthread_self();
    struct timespec start, finish;

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
    {
        fprintf(stderr, "clock_gettime failed (error %s).\n", strerror(errno));
        free(workers);
        return EXIT_FAILURE;
    }

    printf("PID=%ld, main TID=%lu\n", (long)process_id, (unsigned long)main_thread_id);
    printf("Requested worker threads: %zu\n", thread_count);

    size_t created = 0;
    for (size_t i = 0; i < thread_count; ++i)
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

        int error = pthread_create(&workers[i].thread_id, NULL, count_points, &workers[i]);
        if (error != 0)
        {
            fprintf(stderr, "pthread_create failed (error %d).\n", error);
            break;
        }
        ++created;
        printf("Worker %zu: TID=%lu, points=%" PRIu64 "\n",
               i, (unsigned long)workers[i].thread_id, workers[i].sample_count);
    }

    if (created != thread_count)
    {
        if (created > 0)
        {
            for (size_t i = 0; i < created; ++i)
            {
                pthread_join(workers[i].thread_id, NULL);
            }
        }
        free(workers);
        return EXIT_FAILURE;
    }

    for (size_t i = 0; i < thread_count; ++i)
    {
        if (pthread_join(workers[i].thread_id, NULL) != 0)
        {
            fprintf(stderr, "pthread_join failed.\n");
            free(workers);
            return EXIT_FAILURE;
        }
    }

    if (clock_gettime(CLOCK_MONOTONIC, &finish) != 0)
    {
        fprintf(stderr, "clock_gettime failed (error %s).\n", strerror(errno));
        free(workers);
        return EXIT_FAILURE;
    }

    uint64_t total_hits = 0;
    for (size_t i = 0; i < thread_count; ++i)
    {
        total_hits += workers[i].hits;
    }

    const double area = 4.0 * radius * radius * (double)total_hits / (double)sample_total;
    const double elapsed_ms = elapsed_milliseconds(start, finish);
    printf("Points: %" PRIu64 ", inside circle: %" PRIu64 "\n", sample_total, total_hits);
    printf("Estimated circle area: %.10f\n", area);
    printf("Elapsed time: %.3f ms\n", elapsed_ms);

    free(workers);
    return EXIT_SUCCESS;
}
