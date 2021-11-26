#include <assert.h>
#include <errno.h>
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

static const key_t shm_r_sem_key = 0xDED + 3;
static const key_t hsh_r_sem_key = 0xDED + 2;
static const key_t r_sem_key = 0xDED + 1;

static const key_t hsh_w_sem_key = 0xDED - 2;
static const key_t w_sem_key = 0xDED - 1;

static const key_t finish_sem_key = 0xDED;

static const key_t shm_key = 0xDED;

static const size_t shared_buf_sz = 64;
static char *shared_buf;

const static ssize_t sender_handshake = -1;
const static ssize_t receiver_handshake = -2;
const static ssize_t receiver_confirm = -3;

const struct timespec timeout = {
        .tv_sec = 60,
        .tv_nsec = 0,
};

int main() {
    int shm_r_sem_id = semget(shm_r_sem_key, 1, IPC_CREAT | S_IRUSR | S_IWUSR);
    if (shm_r_sem_id == -1) {
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
    int rc = semop(shm_r_sem_id, sembufs, 2);
    if (rc != 0) {
        perror("SYSTEM ERROR: semop failed");
        return EXIT_FAILURE;
    }

    CRIT_SEC_BEGIN
    int hsh_w_sem_id = semget(hsh_w_sem_key, 1, IPC_CREAT | S_IRUSR | S_IWUSR);
    if (hsh_w_sem_id == -1) {
        perror("SYSTEM ERROR: semget failed");
        return EXIT_FAILURE;
    }
    struct sembuf sembuf = {
            .sem_num = 0,
            .sem_op = 1,
            .sem_flg = SEM_UNDO,
    };
    rc = semop(hsh_w_sem_id, &sembuf, 1);
    if (rc != 0) {
        perror("SYSTEM ERROR: semop failed");
        return EXIT_FAILURE;
    }

    int hsh_r_sem_id = semget(hsh_r_sem_key, 1, IPC_CREAT | S_IRUSR | S_IWUSR);
    if (hsh_r_sem_id == -1) {
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

    int shm_id = shmget(shm_key, shared_buf_sz, IPC_CREAT | S_IRUSR | S_IWUSR);
    if (shm_id == -1 && errno != EEXIST) {
        perror("SYSTEM ERROR: shmget failed");
        return EXIT_FAILURE;
    }
    shared_buf = shmat(shm_id, NULL, 0);
    if (shared_buf == (void *) -1) {
        perror("SYSTEM ERROR: shmat failed");
        return EXIT_FAILURE;
    }

    sembuf.sem_op = -1;
    sembuf.sem_flg = 0;
    rc = semtimedop(hsh_r_sem_id, &sembuf, 1, &timeout);
    if (rc != 0) {
        perror("SYSTEM ERROR: semop failed");
        return EXIT_FAILURE;
    }

    CRIT_SEC_BEGIN
    sembuf.sem_op = -1;
    sembuf.sem_flg = 0;
    rc = semop(r_sem_id, &sembuf, 1);
    if (rc != 0) {
        perror("SYSTEM ERROR: semop failed");
        return EXIT_FAILURE;
    }

    CRIT_SEC_BEGIN
    ssize_t status = 0;
    memcpy(&status, shared_buf, sizeof(ssize_t));
    if (status != sender_handshake) {
        fprintf(stderr, "CLIENT ERROR: sender did not handshake correctly\n");
        return EXIT_FAILURE;
    }
    memcpy(shared_buf, &receiver_handshake, sizeof(ssize_t));
    CRIT_SEC_END
    CRIT_SEC_END

    sembuf.sem_op = 1;
    sembuf.sem_flg = SEM_UNDO;
    rc = semop(w_sem_id, &sembuf, 1);
    if (rc != 0) {
        perror("SYSTEM ERROR: semop failed");
        return EXIT_FAILURE;
    }

    while (true) {
        sembuf.sem_op = -1;
        sembuf.sem_flg = 0;
        rc = semtimedop(r_sem_id, &sembuf, 1, &timeout);
        if (rc != 0) {
            perror("SYSTEM ERROR: semop failed");
            return EXIT_FAILURE;
        }

        CRIT_SEC_BEGIN
        ssize_t bytes_read_cnt = 0;
        memcpy(&bytes_read_cnt, shared_buf, sizeof(ssize_t));
        if (bytes_read_cnt == 0) {
            sembuf.sem_op = 1;
            sembuf.sem_flg = 0;
            rc = semop(fin_sem_id, &sembuf, 1);
            if (rc != 0) {
                perror("SYSTEM ERROR: semop failed");
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        }
        if (bytes_read_cnt < 0) {
            fprintf(stderr, "CLIENT ERROR: sender died\n");
            return EXIT_FAILURE;
        }

        ssize_t bytes_written_cnt = write(STDOUT_FILENO, shared_buf +
                                                         sizeof(ssize_t),
                                          bytes_read_cnt);
        if (bytes_written_cnt == -1) {
            perror("SYSTEM ERROR: write failed");
            return EXIT_FAILURE;
        }
        assert(bytes_written_cnt == bytes_read_cnt);
        memcpy(shared_buf, &receiver_confirm, sizeof(ssize_t));
        CRIT_SEC_END

        sembuf.sem_op = 1;
        sembuf.sem_flg = 0;
        rc = semop(w_sem_id, &sembuf, 1);
        if (rc != 0) {
            perror("SYSTEM ERROR: semop failed");
            return EXIT_FAILURE;
        }
    }
    CRIT_SEC_END
}
