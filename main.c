#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

typedef struct
{
    double r;
    int N, extra_N;
    int H;
    unsigned int seed; //random
    pthread_cond_t extra_ready;
} ThreadData;

double randfrom(double min, double max, unsigned int *seed) 
{
    double range = (max - min); 
    double div = RAND_MAX / range;
    return min + (rand_r(seed) / div);

    /*
    double unit = (double)rand_r(seed) / RAND_MAX; [0; 1]
    return min + unit * (max - min); [0; 1] -> [-r; r]
    */
}

/* Один mutex защищает extra_N и сам переход к ожиданию condition variable. */
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
void *work(void *arg)
{
    ThreadData *td = (ThreadData *)arg;
    double r = td->r;
    int N = td->N;
    int H = 0;

    // Generate N dots
    // Count, how many x^2 + y^2 <= r^2
    int i = 0;
    for (; i < N; ++i) {
        double x = randfrom(-r, r, &td->seed), y = randfrom(-r, r, &td->seed);
        if (x*x + y*y <= r*r)
            ++H;
    }
    
    /* Ждём, пока main запишет дополнительное количество точек. */
    pthread_mutex_lock(&lock); // Mutex - защита от чтения td->extra_N во время того, как главный поток в main туда записывает
    while (td->extra_N == -1) // Почему не if? Защита от ложного пробуждения - правильный шаблон для conditional variable
        pthread_cond_wait(&td->extra_ready, &lock); // Отпускаем mutex до пробуждения
    int extra_N = td->extra_N;
    pthread_mutex_unlock(&lock);

    /* Process extra_N dots in a loop (if extra_N == 0, no extra work), добавить попадания к H. */
    for (i = 0; i < extra_N; ++i) {
        double x = randfrom(-r, r, &td->seed), y = randfrom(-r, r, &td->seed);
        if (x*x + y*y <= r*r)
            ++H;
    }

    td->H = H;
    return NULL;
}

int program(double r, int N, int p, int *out_H) {
    int i = 0;

    ThreadData *Threads = (ThreadData *)malloc(p * sizeof(ThreadData));
    if (Threads == NULL)
    {
        fprintf(stderr, "Can't allocate ThreadData array\n");
        return EXIT_FAILURE;
    }

    int N_div_p = N / p;
    int N_modulo_p = N % p;
    int err;
    for (; i < p; ++i)
    {
        Threads[i].r = r;
        Threads[i].N = N_div_p;
        Threads[i].extra_N = -1;
        Threads[i].H = 0;
        Threads[i].seed = i;
        err = pthread_cond_init(&Threads[i].extra_ready, NULL);
        if (err != 0)
        {
            fprintf(stderr, "Can't initialize condition variable: [%s]\n", strerror(err));
            while (i > 0)
                pthread_cond_destroy(&Threads[--i].extra_ready);
            free(Threads);
            return EXIT_FAILURE;
        }
    }
    for (i = 0; i < N_modulo_p; ++i)
    {
        ++Threads[i].N;
    }

    pthread_t tid[p];
    int *created_threads_indices = (int *)malloc(p * sizeof(int));
    if (created_threads_indices == NULL)
    {
        fprintf(stderr, "Can't allocate int array\n");
        for (i = 0; i < p; ++i)
            pthread_cond_destroy(&Threads[i].extra_ready);
        free(Threads);
        return EXIT_FAILURE;
    }

    int created_threads_count = 0;
    int lost_N = 0;
    for (i = 0; i < p; ++i)
    {
        err = pthread_create(&(tid[i]), NULL, &work, &Threads[i]);
        if (err != 0)
        {
            fprintf(stderr, "Can't create thread : [%s]\n", strerror(err));
            lost_N += Threads[i].N;
        }
        else
        {
            created_threads_indices[created_threads_count] = i;
            ++created_threads_count;
        }
    }

    if (created_threads_count == 0)
    {
        fprintf(stderr, "No worker threads were created.\n");
        for (i = 0; i < p; ++i)
            pthread_cond_destroy(&Threads[i].extra_ready);
        free(created_threads_indices);
        free(Threads);
        return EXIT_FAILURE;
    }

    // if lost_N == 0, then every thread gets extra_N = 0 and finishes its work
    N_div_p = lost_N / created_threads_count;
    N_modulo_p = lost_N % created_threads_count;

    /* Записываем все extra_N под mutex, затем будим соответствующие потоки. */
    pthread_mutex_lock(&lock);
    for (i = 0; i < created_threads_count; ++i)
    {
        int thread_index = created_threads_indices[i];
        Threads[thread_index].extra_N = N_div_p;
        if (i < N_modulo_p)
            ++Threads[thread_index].extra_N;
        pthread_cond_signal(&Threads[thread_index].extra_ready);
    }
    pthread_mutex_unlock(&lock);

    int H = 0;
    for (i = 0; i < created_threads_count; ++i)
    {
        int thread_index = created_threads_indices[i];
        err = pthread_join(tid[thread_index], NULL);
        if (err != 0)
            fprintf(stderr, "Can't wait for thread : [%s]\n", strerror(err));
        else
            H += Threads[thread_index].H;
    }

    for (i = 0; i < p; ++i)
        pthread_cond_destroy(&Threads[i].extra_ready);
    free(created_threads_indices);
    free(Threads);
    *out_H = H;
    return EXIT_SUCCESS;
}

int main(void)
{
    double r;
    int N, p;
    if (scanf("%lf %d %d", &r, &N, &p) != 3 || !(r > 0 && N > 0 && p > 0 && p <= N))
    {
        fprintf(stderr, "Некорректный ввод\n");
        return EXIT_FAILURE;
    }
 
    struct timespec start, end;

    int H1;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int p1 = program(r, N, 1, &H1);
    clock_gettime(CLOCK_MONOTONIC, &end);
    if (p1 != 0) {
        return EXIT_FAILURE;
    }
    double T1 = (end.tv_sec - start.tv_sec) + 
                     (end.tv_nsec - start.tv_nsec) / 1000000000.0; // seconds

    int Hp;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int p2 = program(r, N, p, &Hp);
    clock_gettime(CLOCK_MONOTONIC, &end);
    if (p2 != 0) {
        return EXIT_FAILURE;
    }
    pthread_mutex_destroy(&lock);
    double Tp = (end.tv_sec - start.tv_sec) + 
                     (end.tv_nsec - start.tv_nsec) / 1000000000.0; // seconds
    double Sp = T1 / Tp;
    double Ep = Sp / p;

    printf("Result with 1 thread: %d\nResult with p threads: %d\n", H1, Hp);
    printf("T1: %lf\nTp: %lf\nSp = T1 / Tp: %lf\nEp = Sp/p: %lf", T1, Tp, Sp, Ep);

    return EXIT_SUCCESS;
}