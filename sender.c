#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/shm.h>
#include <sys/sem.h>

#define CRIT_SEC_BEGIN
#define CRIT_SEC_END

static const key_t shm_w_sem_key = 0xDED - 3;
static const key_t hsh_w_sem_key = 0xDED - 2;
static const key_t w_sem_key = 0xDED - 1;

static const key_t shm_r_sem_key = 0xDED + 3;
static const key_t hsh_r_sem_key = 0xDED + 2;
static const key_t r_sem_key = 0xDED + 1;

static const key_t finish_sem_key = 0xDED;

static const key_t shm_key = 0xDED;

static const size_t shared_mem_sz = 64;
static char *shared_mem;

const static ssize_t sender_handshake = -1;
const static ssize_t receiver_handshake = -2;
const static ssize_t receiver_confirm = -3;

const struct timespec timeout = {
        .tv_sec = 60,
        .tv_nsec = 0,
};

int main(int argc, const char *const argv[]) {
    --argc;
    ++argv;

    if (argc != 1) {
        printf("USAGE: shared-memory-sender <path_to_file>\n");
        return EXIT_FAILURE;
    }

    const char *file_name = argv[0];
    int file_fd = open(file_name, O_RDONLY);
    if (file_fd == -1) {
        perror("SYSTEM ERROR: open failed");
        return EXIT_FAILURE;
    }

    int shm_r_sem_id = semget(shm_r_sem_key, 1, IPC_CREAT | S_IRUSR | S_IWUSR);
    if (shm_r_sem_id == -1) {
        perror("SYSTEM ERROR: semget failed");
        return EXIT_FAILURE;
    }

    int shm_w_sem_id = semget(shm_w_sem_key, 1, IPC_CREAT | S_IRUSR | S_IWUSR);
    if (shm_w_sem_id == -1) {
        perror("SYSTEM ERROR: semget failed");
        return EXIT_FAILURE;
    }
    struct sembuf sembufs[] = {{
                                       .sem_num = 0,
                                       .sem_op = 0,
                                       .sem_flg = 0,
                               },
                               {
                                       .sem_num = 0,
                                       .sem_op = 1,
                                       .sem_flg = SEM_UNDO,
                               }};
    int rc = semop(shm_w_sem_id, sembufs, 2);
    if (rc != 0) {
        perror("SYSTEM ERROR: semop failed");
        return EXIT_FAILURE;
    }

    CRIT_SEC_BEGIN
    int hsh_r_sem_id = semget(hsh_r_sem_key, 1, IPC_CREAT | S_IRUSR | S_IWUSR);
    if (hsh_r_sem_id == -1) {
        perror("SYSTEM ERROR: semget failed");
        return EXIT_FAILURE;
    }
    struct sembuf sembuf = {
            .sem_num = 0,
            .sem_op = 1,
            .sem_flg = SEM_UNDO,
    };
    rc = semop(hsh_r_sem_id, &sembuf, 1);
    if (rc != 0) {
        perror("SYSTEM ERROR: semop failed");
        return EXIT_FAILURE;
    }

    int hsh_w_sem_id = semget(hsh_w_sem_key, 1, IPC_CREAT | S_IRUSR | S_IWUSR);
    if (hsh_w_sem_id == -1) {
        perror("SYSTEM ERROR: semget failed");
        return EXIT_FAILURE;
    }

    int r_sem_id = semget(r_sem_key, 1, IPC_CREAT | S_IRUSR | S_IWUSR);
    if (r_sem_id == -1) {
        perror("SYSTEM ERROR: semget failed");
        return EXIT_FAILURE;
    }

    int w_sem_id = semget(w_sem_key, 1, IPC_CREAT | S_IRUSR | S_IWUSR);
    if (w_sem_id == -1) {
        perror("SYSTEM ERROR: semget failed");
        return EXIT_FAILURE;
    }

    int fin_sem_id = semget(finish_sem_key, 1, IPC_CREAT | S_IRUSR | S_IWUSR);
    if (fin_sem_id == -1) {
        perror("SYSTEM ERROR: semget failed");
        return EXIT_FAILURE;
    }

    int shm_id = shmget(shm_key, shared_mem_sz, IPC_CREAT | S_IRUSR | S_IWUSR);
    if (shm_id == -1) {
        perror("SYSTEM ERROR: shmget failed");
        return EXIT_FAILURE;
    }
    shared_mem = shmat(shm_id, NULL, 0);
    if (shared_mem == (void *) -1) {
        perror("SYSTEM ERROR: shmat failed");
        return EXIT_FAILURE;
    }

    sembuf.sem_op = -1;
    sembuf.sem_flg = 0;
    rc = semtimedop(hsh_w_sem_id, &sembuf, 1, &timeout);
    if (rc != 0) {
        perror("SYSTEM ERROR: semop failed");
        return EXIT_FAILURE;
    }

    CRIT_SEC_BEGIN
    rc = semctl(fin_sem_id, 0, SETVAL, 0);
    if (rc == -1) {
        perror("SYSTEM ERROR: semctl failed");
        return EXIT_FAILURE;
    }

    memcpy(shared_mem, &sender_handshake, sizeof(ssize_t));
    CRIT_SEC_END

    sembuf.sem_op = 1;
    sembuf.sem_flg = SEM_UNDO;
    rc = semop(r_sem_id, &sembuf, 1);
    if (rc != 0) {
        perror("SYSTEM ERROR: semop failed");
        return EXIT_FAILURE;
    }

    bool handshaked = false;
    while (true) {
        sembuf.sem_op = -1;
        sembuf.sem_flg = 0;
        rc = semtimedop(w_sem_id, &sembuf, 1, &timeout);
        if (rc != 0) {
            perror("SYSTEM ERROR: semop failed");
            return EXIT_FAILURE;
        }

        CRIT_SEC_BEGIN
        ssize_t status = 0;
        memcpy(&status, shared_mem, sizeof(ssize_t));
        if (!handshaked) {
            if (status != receiver_handshake) {
                fprintf(stderr, "CLIENT ERROR: receiver did not handshake correctly\n");
                return EXIT_FAILURE;
            }
            handshaked = true;
        } else {
            if (status != receiver_confirm) {
                fprintf(stderr, "CLIENT ERROR: receiver died\n");
                return EXIT_FAILURE;
            }
        }

        ssize_t bytes_read_cnt = read(file_fd, shared_mem + sizeof(ssize_t),
                                      shared_mem_sz - sizeof(ssize_t));
        if (bytes_read_cnt == -1) {
            perror("SYSTEM ERROR: read failed");
            return EXIT_FAILURE;
        }
        memcpy(shared_mem, &bytes_read_cnt, sizeof(ssize_t));
        CRIT_SEC_END

        sembuf.sem_op = 1;
        sembuf.sem_flg = 0;
        rc = semop(r_sem_id, &sembuf, 1);
        if (rc != 0) {
            perror("SYSTEM ERROR: semop failed");
            return EXIT_FAILURE;
        }

        if (bytes_read_cnt == 0) {
            sembuf.sem_op = -1;
            sembuf.sem_flg = 0;
            rc = semtimedop(fin_sem_id, &sembuf, 1, &timeout);
            if (rc != 0) {
                perror("SYSTEM ERROR: semop failed");
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        }
    }
    CRIT_SEC_END
}
