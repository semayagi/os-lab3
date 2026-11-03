#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef struct
{
    double r;
    unsigned int N;
    unsigned int H;
} ThreadData;

void *work(void *arg)
{
    ThreadData *td = (ThreadData *)arg;
    double r = td->r;
    unsigned int N = td->N;
    unsigned int H = td->H; // 0

    // Generate N dots
    // Count, how many x^2 + y^2 <= r^2

    return NULL;
}

int main(void)
{
    double r;
    unsigned int N, p;
    if (scanf("%lf %u %u", &r, &N, &p) != 3 || !(r > 0 && N > 0 && p > 0 && p <= N))
    {
        fprintf(stderr, "Некорректный ввод\n");
        return EXIT_FAILURE;
    }

    int i = 0;

    ThreadData *Threads = (ThreadData *)malloc(p * sizeof(ThreadData));
    if (Threads == NULL)
    {
        fprintf(stderr, "Can't allocate ThreadData array\n");
        return EXIT_FAILURE;
    }

    int N_div_p = N / p;
    int N_modulo_p = N % p;
    for (; i < p; ++i)
    {
        Threads[i].r = r;
        Threads[i].N = N_div_p;
        Threads[i].H = 0;
    }
    for (i = 0; i < N_modulo_p; ++i)
    {
        ++Threads[i].N;
    }

    pthread_t tid[p];
    int err;

    for (i = 0; i < p; ++i)
    {
        err = pthread_create(&(tid[i]), NULL, &work, &Threads[i]);
        if (err != 0)
        {
            fprintf(stderr, "Can't create thread : [%s]\n", strerror(err));
            tid[i] = NULL;
        }
    }

    unsigned int H = 0;
    for (i = 0; i < p; ++i)
    {
        if (tid[i] == NULL)
            continue;

        err = pthread_join(tid[i], NULL);
        if (err != 0)
            printf("\nCan't wait for thread : [%s]", strerror(err));
        else
            H += Threads[i].H;
    }
    free(Threads);
    printf("%u", H);
}