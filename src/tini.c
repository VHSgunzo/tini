/* See LICENSE file for copyright and license details. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L // For functions potentially used by proctree code
#define _DEFAULT_SOURCE         // For functions potentially used by proctree code

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <libgen.h>
#include <dirent.h>
#include <ctype.h>
#include <stddef.h>
#include <limits.h>

#include "tiniConfig.h"
#include "tiniLicense.h"

// --- Tini Configuration and Logging ---
#if TINI_MINIMAL
#define PRINT_FATAL(...)                         fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");
#define PRINT_WARNING(...)  if (verbosity > 0) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); }
#define PRINT_INFO(...)     if (verbosity > 1) { fprintf(stdout, __VA_ARGS__); fprintf(stdout, "\n"); }
#define PRINT_DEBUG(...)    if (verbosity > 2) { fprintf(stdout, __VA_ARGS__); fprintf(stdout, "\n"); }
#define PRINT_TRACE(...)    if (verbosity > 3) { fprintf(stdout, __VA_ARGS__); fprintf(stdout, "\n"); }
#define DEFAULT_VERBOSITY 0
#else
#define PRINT_FATAL(...)                         fprintf(stderr, "[FATAL tini (%i)] ", getpid()); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");
#define PRINT_WARNING(...)  if (verbosity > 0) { fprintf(stderr, "[WARN  tini (%i)] ", getpid()); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); }
#define PRINT_INFO(...)     if (verbosity > 1) { fprintf(stdout, "[INFO  tini (%i)] ", getpid()); fprintf(stdout, __VA_ARGS__); fprintf(stdout, "\n"); }
#define PRINT_DEBUG(...)    if (verbosity > 2) { fprintf(stdout, "[DEBUG tini (%i)] ", getpid()); fprintf(stdout, __VA_ARGS__); fprintf(stdout, "\n"); }
#define PRINT_TRACE(...)    if (verbosity > 3) { fprintf(stdout, "[TRACE tini (%i)] ", getpid()); fprintf(stdout, __VA_ARGS__); fprintf(stdout, "\n"); }
#define DEFAULT_VERBOSITY 1
#endif

#define ARRAY_LEN(x)  (sizeof(x) / sizeof((x)[0]))

#define INT32_BITFIELD_SET(F, i)     ( F[(i / 32)] |=  (1 << (i % 32)) )
#define INT32_BITFIELD_CLEAR(F, i)   ( F[(i / 32)] &= ~(1 << (i % 32)) )
#define INT32_BITFIELD_TEST(F, i)    ( F[(i / 32)] &   (1 << (i % 32)) )
#define INT32_BITFIELD_CHECK_BOUNDS(F, i) do {  assert(i >= 0); assert(ARRAY_LEN(F) > (size_t) (i / 32)); } while(0) // Use size_t cast

#define STATUS_MAX 255
#define STATUS_MIN 0

// --- Proctree Configuration (Internal) ---
#define HASH_TABLE_SIZE 4096
#define INITIAL_QUEUE_CAPACITY 16
#define INITIAL_DESCENDANTS_CAPACITY 16
#define MAX_PATH_LEN 256 // Max length for /proc/[pid]/status path
#define MAX_LINE_LEN 256 // Max line length in status file

// --- Proctree Data Structures ---
typedef struct {
    pid_t *pids;  // Dynamically allocated array of descendant PIDs
    size_t count; // Number of PIDs in the array
} DescendantList;

// Linked list node for children
typedef struct ChildNode {
    pid_t child_pid;
    struct ChildNode *next;
} ChildNode;

// Entry in the parent map hash table
typedef struct ParentMapEntry {
    pid_t ppid;
    ChildNode *children_head;
    struct ParentMapEntry *next;
} ParentMapEntry;

// Visited Set node (for BFS)
typedef struct VisitedNode {
    pid_t pid;
    struct VisitedNode *next;
} VisitedNode;

// Dynamic Array based Queue for BFS
typedef struct PidQueue {
    pid_t *pids;
    size_t capacity;
    size_t count;
    size_t head;
} PidQueue;


// --- Tini Data Structures ---
typedef struct {
   sigset_t* const sigmask_ptr;
   struct sigaction* const sigttin_action_ptr;
   struct sigaction* const sigttou_action_ptr;
} signal_configuration_t;


// --- Tini Globals and Constants ---
static const struct {
   char *const name;
   int number;
} signal_names[] = {
    { "SIGHUP", SIGHUP },
    { "SIGINT", SIGINT },
    { "SIGQUIT", SIGQUIT },
    { "SIGILL", SIGILL },
    { "SIGTRAP", SIGTRAP },
    { "SIGABRT", SIGABRT },
    { "SIGBUS", SIGBUS },
    { "SIGFPE", SIGFPE },
    { "SIGKILL", SIGKILL },
    { "SIGUSR1", SIGUSR1 },
    { "SIGSEGV", SIGSEGV },
    { "SIGUSR2", SIGUSR2 },
    { "SIGPIPE", SIGPIPE },
    { "SIGALRM", SIGALRM },
    { "SIGTERM", SIGTERM },
    { "SIGCHLD", SIGCHLD },
    { "SIGCONT", SIGCONT },
    { "SIGSTOP", SIGSTOP },
    { "SIGTSTP", SIGTSTP },
    { "SIGTTIN", SIGTTIN },
    { "SIGTTOU", SIGTTOU },
    { "SIGURG", SIGURG },
    { "SIGXCPU", SIGXCPU },
    { "SIGXFSZ", SIGXFSZ },
    { "SIGVTALRM", SIGVTALRM },
    { "SIGPROF", SIGPROF },
    { "SIGWINCH", SIGWINCH },
    { "SIGSYS", SIGSYS },
};

static unsigned int verbosity = DEFAULT_VERBOSITY;
static int32_t expect_status[(STATUS_MAX - STATUS_MIN + 1) / 32];

#ifdef PR_SET_CHILD_SUBREAPER
#define HAS_SUBREAPER 1
#define OPT_STRING "p:hvwgle:sak:"
#define SUBREAPER_ENV_VAR "TINI_SUBREAPER"
#else
#define HAS_SUBREAPER 0
#define OPT_STRING "p:hvwgle:ak:"
#endif

#define VERBOSITY_ENV_VAR "TINI_VERBOSITY"
#define KILL_PROCESS_GROUP_GROUP_ENV_VAR "TINI_KILL_PROCESS_GROUP"

#define TINI_VERSION_STRING "tini version " TINI_VERSION TINI_GIT

#if HAS_SUBREAPER
static unsigned int subreaper = 0;
#endif
static unsigned int parent_death_signal = 0;
static unsigned int kill_process_group = 0;
static unsigned int warn_on_reap = 0;
static unsigned int kill_descendants_on_exit = 0;
static int descendant_signal_to_send = SIGKILL;


static struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };

static const char reaper_warning[] = "Tini is not running as PID 1 "
#if HAS_SUBREAPER
       "and isn't registered as a child subreaper"
#endif
".\n\
Zombie processes will not be re-parented to Tini, so zombie reaping won't work.\n\
To fix the problem, "
#if HAS_SUBREAPER
#ifndef TINI_MINIMAL
"use the -s option or "
#endif
"set the environment variable " SUBREAPER_ENV_VAR " to register Tini as a child subreaper, or "
#endif
"run Tini as PID 1.";

// --- Proctree Internal Helper Function Declarations ---
// Mark them static as they are internal implementation details
static unsigned int hash_pid(pid_t pid);
static int add_child_to_map(ParentMapEntry **map, pid_t ppid, pid_t child_pid);
static ChildNode *get_children(ParentMapEntry **map, pid_t ppid);
static void free_parent_map(ParentMapEntry **map);
static int build_parent_map_from_proc(ParentMapEntry **map);
static int add_to_visited(VisitedNode **set, pid_t pid);
static void clear_visited_set(VisitedNode **set);
static void free_visited_set(VisitedNode **set); // Frees nodes AND clears
static int init_queue(PidQueue *q, size_t initial_capacity);
static int enqueue(PidQueue *q, pid_t pid);
static pid_t dequeue(PidQueue *q);
static void free_queue(PidQueue *q);
static int compare_pids(const void *a, const void *b);
static DescendantList find_all_descendants_internal(pid_t start_pid, ParentMapEntry **parent_map);
// --- Proctree Public API Function Declarations (Marked static now) ---
static DescendantList get_process_descendants(pid_t parent_pid);
static void free_descendant_list(DescendantList *list);

// --- Proctree Internal Function Implementations ---

static unsigned int hash_pid(pid_t pid) {
    return (unsigned int)pid % HASH_TABLE_SIZE;
}

static int add_child_to_map(ParentMapEntry **map, pid_t ppid, pid_t child_pid) {
    unsigned int index = hash_pid(ppid);
    ParentMapEntry *entry = map[index];
    ParentMapEntry *prev = NULL;

    while (entry != NULL && entry->ppid != ppid) {
        prev = entry;
        entry = entry->next;
    }

    if (entry == NULL) {
        entry = malloc(sizeof(ParentMapEntry));
        if (!entry) return -1; // Error reported by caller
        entry->ppid = ppid;
        entry->children_head = NULL;
        entry->next = NULL;
        if (prev == NULL) map[index] = entry;
        else prev->next = entry;
    }

    ChildNode *new_child = malloc(sizeof(ChildNode));
    if (!new_child) return -1; // Error reported by caller
    new_child->child_pid = child_pid;
    new_child->next = entry->children_head;
    entry->children_head = new_child;
    return 0;
}

static ChildNode *get_children(ParentMapEntry **map, pid_t ppid) {
    unsigned int index = hash_pid(ppid);
    ParentMapEntry *entry = map[index];
    while (entry != NULL) {
        if (entry->ppid == ppid) return entry->children_head;
        entry = entry->next;
    }
    return NULL;
}

static void free_parent_map(ParentMapEntry **map) {
    if (!map) return;
    for (int i = 0; i < HASH_TABLE_SIZE; ++i) {
        ParentMapEntry *entry = map[i];
        while (entry != NULL) {
            ParentMapEntry *next_entry = entry->next;
            ChildNode *child = entry->children_head;
            while (child != NULL) {
                ChildNode *next_child = child->next;
                free(child);
                child = next_child;
            }
            free(entry);
            entry = next_entry;
        }
        map[i] = NULL; // Defensive
    }
    // Don't free the 'map' array itself here, it's allocated on stack or heap
    // by the caller (get_process_descendants)
}

static int build_parent_map_from_proc(ParentMapEntry **map) {
    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) {
        PRINT_WARNING("Failed to open /proc: %s", strerror(errno)); // Use Tini logging
        return -1;
    }

    struct dirent *entry;
    char path_buffer[MAX_PATH_LEN];
    char line_buffer[MAX_LINE_LEN];
    int success = 0; // Indicate overall success

    while ((entry = readdir(proc_dir)) != NULL) {
        // Check if directory name is a number (PID)
        if (entry->d_type == DT_DIR && isdigit(entry->d_name[0])) {
            int required_len = snprintf(path_buffer, sizeof(path_buffer), "/proc/%s/status", entry->d_name);
            if (required_len < 0 || (size_t)required_len >= sizeof(path_buffer)) {
                PRINT_TRACE("Path for PID %s too long or encoding error, skipping.", entry->d_name);
                continue;
            }

            FILE *status_file = fopen(path_buffer, "r");
            if (!status_file) {
                 // Process might have disappeared, permissions issue, etc. - often benign
                 // PRINT_TRACE("Could not open %s: %s", path_buffer, strerror(errno));
                 continue;
            }

            pid_t pid = -1;
            pid_t ppid = -1;
            while (fgets(line_buffer, sizeof(line_buffer), status_file)) {
                if (pid == -1 && strncmp(line_buffer, "Pid:", 4) == 0) {
                    // Use strtol for slightly more robust parsing than sscanf
                    char *endptr;
                    long val = strtol(line_buffer + 4, &endptr, 10);
                    if (endptr != line_buffer + 4 && val > 0) pid = (pid_t)val;
                } else if (ppid == -1 && strncmp(line_buffer, "PPid:", 5) == 0) {
                     char *endptr;
                     long val = strtol(line_buffer + 5, &endptr, 10);
                     if (endptr != line_buffer + 5 && val >= 0) ppid = (pid_t)val;
                }
                if (pid != -1 && ppid != -1) break;
            }
            fclose(status_file);

            if (pid > 0 && ppid >= 0) {
                if (add_child_to_map(map, ppid, pid) != 0) {
                    // Allocation error!
                    PRINT_WARNING("Memory allocation failed while building process map."); // Use Tini logging
                    success = -1; // Mark as failed
                    break; // Stop processing /proc
                }
            } else {
                 PRINT_TRACE("Could not parse PID/PPID from %s", path_buffer);
            }
        }
    }

    closedir(proc_dir);
    return success;
}


static int add_to_visited(VisitedNode **set, pid_t pid) {
    unsigned int index = hash_pid(pid);
    VisitedNode *node = set[index];
    while (node != NULL) {
        if (node->pid == pid) return 0; // Already visited
        node = node->next;
    }
    VisitedNode *new_node = malloc(sizeof(VisitedNode));
    if (!new_node) return -1; // Allocation error
    new_node->pid = pid;
    new_node->next = set[index];
    set[index] = new_node;
    return 1; // Added successfully
}

static void clear_visited_set(VisitedNode **set) {
    if (!set) return;
    for (int i = 0; i < HASH_TABLE_SIZE; ++i) {
        VisitedNode *node = set[i];
        while (node != NULL) {
            VisitedNode *next_node = node->next;
            free(node);
            node = next_node;
        }
        set[i] = NULL;
    }
}

static void free_visited_set(VisitedNode **set) {
    clear_visited_set(set);
    // Don't free the 'set' array itself, assume allocated elsewhere
}


static int init_queue(PidQueue *q, size_t initial_capacity) {
    q->pids = malloc(initial_capacity * sizeof(pid_t));
    if (!q->pids) return -1;
    q->capacity = initial_capacity;
    q->count = 0;
    q->head = 0;
    return 0;
}

static int enqueue(PidQueue *q, pid_t pid) {
     // Check if resize needed (using simple linear buffer logic here)
    if (q->count >= q->capacity) {
        size_t new_capacity = q->capacity == 0 ? INITIAL_QUEUE_CAPACITY : q->capacity * 2;
        // Handle wrap-around if head is not 0 for circular buffer (more complex)
        // Simple linear resize:
        if (q->head > 0) {
             // If head is not at 0, shift elements to the beginning before realloc
             memmove(q->pids, q->pids + q->head, q->count * sizeof(pid_t));
             q->head = 0; // Reset head after shifting
        }

        pid_t *new_pids = realloc(q->pids, new_capacity * sizeof(pid_t));
        if (!new_pids) return -1; // Allocation error
        q->pids = new_pids;
        q->capacity = new_capacity;
    }
    // Calculate index to add (tail) - simple linear buffer calculation
    size_t tail_index = q->head + q->count; // No modulo needed for simple linear
    q->pids[tail_index] = pid;
    q->count++;
    return 0;
}

static pid_t dequeue(PidQueue *q) {
    if (q->count == 0) return -1; // Empty queue
    pid_t pid = q->pids[q->head];
    q->head++; // Move head forward
    q->count--;
    // Optional: Reset head to 0 if queue becomes empty to prevent large offset drift
    if (q->count == 0) {
        q->head = 0;
    }
    return pid;
}


static void free_queue(PidQueue *q) {
    free(q->pids);
    q->pids = NULL;
    q->capacity = 0;
    q->count = 0;
    q->head = 0;
}

static int compare_pids(const void *a, const void *b) {
    pid_t pid_a = *(const pid_t *)a;
    pid_t pid_b = *(const pid_t *)b;
    if (pid_a < pid_b) return -1;
    if (pid_a > pid_b) return 1;
    return 0;
}

static DescendantList find_all_descendants_internal(pid_t start_pid, ParentMapEntry **parent_map) {
    DescendantList result = {NULL, 0}; // Initialize to error/empty state
    PidQueue queue;
    VisitedNode *visited_set[HASH_TABLE_SIZE] = {NULL}; // Local visited set for this run

    size_t desc_capacity = INITIAL_DESCENDANTS_CAPACITY;
    result.pids = malloc(desc_capacity * sizeof(pid_t));
    if (!result.pids) {
        PRINT_WARNING("Failed to allocate initial descendants array: %s", strerror(errno));
        return result; // Return {NULL, 0}
    }
    result.count = 0;

    if (init_queue(&queue, INITIAL_QUEUE_CAPACITY) != 0) {
        PRINT_WARNING("Failed to initialize BFS queue: %s", strerror(errno));
        free(result.pids);
        result.pids = NULL;
        return result; // Return {NULL, 0}
    }

    ChildNode *child = get_children(parent_map, start_pid);
    while (child != NULL) {
        int added = add_to_visited(visited_set, child->child_pid);
        if (added == 1) {
            if (result.count >= desc_capacity) {
                desc_capacity *= 2;
                pid_t *new_desc = realloc(result.pids, desc_capacity * sizeof(pid_t));
                if (!new_desc) {
                    PRINT_WARNING("Failed to realloc descendants array: %s", strerror(errno));
                    goto bfs_cleanup_error;
                }
                result.pids = new_desc;
            }
            result.pids[result.count++] = child->child_pid;

            if (enqueue(&queue, child->child_pid) != 0) {
                PRINT_WARNING("Failed to enqueue child PID %d: %s", child->child_pid, strerror(errno));
                goto bfs_cleanup_error;
            }
        } else if (added == -1) {
            PRINT_WARNING("Failed to add child PID %d to visited set: %s", child->child_pid, strerror(errno));
            goto bfs_cleanup_error;
        }
        child = child->next;
    }

    pid_t current_pid;
    while ((current_pid = dequeue(&queue)) != -1) {
        ChildNode *grand_child = get_children(parent_map, current_pid);
        while (grand_child != NULL) {
            int added = add_to_visited(visited_set, grand_child->child_pid);
            if (added == 1) {
                if (result.count >= desc_capacity) {
                    desc_capacity *= 2;
                    pid_t *new_desc = realloc(result.pids, desc_capacity * sizeof(pid_t));
                    if (!new_desc) {
                        PRINT_WARNING("Failed to realloc descendants array: %s", strerror(errno));
                        goto bfs_cleanup_error;
                    }
                    result.pids = new_desc;
                }
                result.pids[result.count++] = grand_child->child_pid;

                if (enqueue(&queue, grand_child->child_pid) != 0) {
                    PRINT_WARNING("Failed to enqueue grandchild PID %d: %s", grand_child->child_pid, strerror(errno));
                    goto bfs_cleanup_error;
                }
            } else if (added == -1) {
                PRINT_WARNING("Failed to add grandchild PID %d to visited set: %s", grand_child->child_pid, strerror(errno));
                goto bfs_cleanup_error;
            }
            grand_child = grand_child->next;
        }
    }

    // BFS finished successfully
    free_queue(&queue);
    free_visited_set(visited_set);

    if (result.count > 0 && result.count < desc_capacity) {
         pid_t *final_pids = realloc(result.pids, result.count * sizeof(pid_t));
         if(final_pids != NULL) {
            result.pids = final_pids;
         } // Keep larger buffer if realloc fails
    } else if (result.count == 0) {
        free(result.pids);
        result.pids = NULL;
    }

    if (result.count > 1) {
        qsort(result.pids, result.count, sizeof(pid_t), compare_pids);
    }

    return result;

bfs_cleanup_error:
    free_queue(&queue);
    free_visited_set(visited_set);
    free(result.pids);
    result.pids = NULL;
    result.count = 0;
    return result; // Return {NULL, 0}
}

// --- Proctree Public API Implementations (Marked static now) ---

static DescendantList get_process_descendants(pid_t parent_pid) {
    DescendantList result = {NULL, 0};
    ParentMapEntry **parent_map = calloc(HASH_TABLE_SIZE, sizeof(ParentMapEntry *));
    if (!parent_map) {
        PRINT_WARNING("Failed to allocate parent map hash table: %s", strerror(errno));
        return result;
    }

    if (build_parent_map_from_proc(parent_map) != 0) {
        PRINT_DEBUG("Failed to build process map completely (error logged).");
        // Continue anyway, might have partial map
    }

    result = find_all_descendants_internal(parent_pid, parent_map);

    free_parent_map(parent_map);
    free(parent_map); // Free the hash table array itself

    return result;
}

static void free_descendant_list(DescendantList *list) {
    if (list && list->pids) {
        free(list->pids);
        list->pids = NULL; // Avoid double free
    }
    if (list) {
       list->count = 0; // Reset count
    }
}


// --- Tini Functions ---

// Keep restore_signals, isolate_child, spawn functions...
int restore_signals(const signal_configuration_t* const sigconf_ptr) {
	if (sigprocmask(SIG_SETMASK, sigconf_ptr->sigmask_ptr, NULL)) {
		PRINT_FATAL("Restoring child signal mask failed: '%s'", strerror(errno));
		return 1;
	}
	if (sigaction(SIGTTIN, sigconf_ptr->sigttin_action_ptr, NULL)) {
		PRINT_FATAL("Restoring SIGTTIN handler failed: '%s'", strerror((errno)));
		return 1;
	}
	if (sigaction(SIGTTOU, sigconf_ptr->sigttou_action_ptr, NULL)) {
		PRINT_FATAL("Restoring SIGTTOU handler failed: '%s'", strerror((errno)));
		return 1;
	}
	return 0;
}

int isolate_child(void) {
	if (setpgid(0, 0) < 0) {
		PRINT_FATAL("setpgid failed: %s", strerror(errno));
		return 1;
	}
	if (tcsetpgrp(STDIN_FILENO, getpgrp())) {
		if (errno == ENOTTY) {
			PRINT_DEBUG("tcsetpgrp failed: no tty (ok to proceed)");
		} else if (errno == ENXIO) {
			PRINT_DEBUG("tcsetpgrp failed: no such device (ok to proceed)");
		} else {
			PRINT_FATAL("tcsetpgrp failed: %s", strerror(errno));
			return 1;
		}
	}
	return 0;
}

int spawn(const signal_configuration_t* const sigconf_ptr, char* const argv[], int* const child_pid_ptr) {
	pid_t pid = fork();
	if (pid < 0) {
		PRINT_FATAL("fork failed: %s", strerror(errno));
		return 1;
	} else if (pid == 0) { // Child process
		if (isolate_child()) {
			_exit(1); // Use _exit in child after fork
		}
		if (restore_signals(sigconf_ptr)) {
			_exit(1);
		}
		execvp(argv[0], argv);
		// execvp only returns on error
		int status = 1;
		switch (errno) {
			case ENOENT: status = 127; break;
			case EACCES: status = 126; break;
		}
		PRINT_FATAL("exec %s failed: %s", argv[0], strerror(errno));
		_exit(status);
	} else { // Parent process
		PRINT_INFO("Spawned child process '%s' with pid '%i'", argv[0], pid);
		*child_pid_ptr = pid;
		return 0;
	}
}


// Keep print_usage, print_license...
void print_usage(char* const name, FILE* const file) {
	fprintf(file, "%s (%s)\n", basename(name), TINI_VERSION_STRING);
#if TINI_MINIMAL
	fprintf(file, "Usage: %s PROGRAM [ARGS] | --version\n\n", basename(name));
#else
	fprintf(file, "Usage: %s [OPTIONS] PROGRAM -- [ARGS] | --version\n\n", basename(name));
#endif
	fprintf(file, "Execute a program under the supervision of a valid init process (%s)\n\n", basename(name));
	fprintf(file, "Command line options:\n\n");
	fprintf(file, "  --version: Show version and exit.\n");
#if TINI_MINIMAL
#else
	fprintf(file, "  -h: Show this help message and exit.\n");
#if HAS_SUBREAPER
	fprintf(file, "  -s: Register as a process subreaper (requires Linux >= 3.4).\n");
#endif
	fprintf(file, "  -p SIGNAL: Trigger SIGNAL on tini when parent dies, e.g. \"-p SIGKILL\".\n");
	fprintf(file, "  -v: Generate more verbose output. Repeat up to 3 times.\n");
	fprintf(file, "  -w: Print a warning when processes are getting reaped.\n");
	fprintf(file, "  -g: Send signals to the child's process group instead of the child.\n");
	fprintf(file, "  -e  EXIT_CODE: Remap EXIT_CODE (from 0 to 255) to 0 (can be repeated).\n");
    fprintf(file, "  -a: Kill all descendant processes when the main child exits.\n");
    fprintf(file, "  -k  SIGNAL: Signal to send to descendants with -a (default: SIGKILL, e.g. \"-k SIGTERM\").\n");
	fprintf(file, "  -l: Show license and exit.\n");
#endif
	fprintf(file, "\nEnvironment variables:\n\n");
#if HAS_SUBREAPER
	fprintf(file, "  %s: Register as a process subreaper (requires Linux >= 3.4).\n", SUBREAPER_ENV_VAR);
#endif
	fprintf(file, "  %s: Set the verbosity level (default: %d).\n", VERBOSITY_ENV_VAR, DEFAULT_VERBOSITY);
	fprintf(file, "  %s: Send signals to the child's process group.\n", KILL_PROCESS_GROUP_GROUP_ENV_VAR);
	fprintf(file, "\n");
}

void print_license(FILE* const file) {
    // size_t because fwrite returns size_t
    size_t written = fwrite(LICENSE, sizeof(char), LICENSE_len, file);
    if(LICENSE_len > written) {
       PRINT_WARNING("Failed to write full license: %s", strerror(errno));
    }
}


// Keep set_pdeathsig, add_expect_status...
int set_pdeathsig(char* const arg) {
	size_t i;
	for (i = 0; i < ARRAY_LEN(signal_names); i++) {
		if (strcmp(signal_names[i].name, arg) == 0) {
			parent_death_signal = signal_names[i].number; return 0;
		}
	}
	char *endptr = NULL; long sig_num = strtol(arg, &endptr, 10);
    if (endptr != NULL && *endptr == '\0' && sig_num > 0 && sig_num < NSIG) {
        parent_death_signal = (int)sig_num; PRINT_DEBUG("Parsed parent death signal number: %d", parent_death_signal); return 0;
    }
	return 1;
}

int add_expect_status(char* arg) {
	long status = 0; char* endptr = NULL; status = strtol(arg, &endptr, 10);
	if ((endptr == NULL) || (*endptr != 0)) { return 1; }
	if ((status < STATUS_MIN) || (status > STATUS_MAX)) { return 1; }
	INT32_BITFIELD_CHECK_BOUNDS(expect_status, status);
	INT32_BITFIELD_SET(expect_status, status); return 0;
}

int set_descendant_signal(char* const arg) {
	size_t i;
	for (i = 0; i < ARRAY_LEN(signal_names); i++) {
		if (strcmp(signal_names[i].name, arg) == 0) {
			descendant_signal_to_send = signal_names[i].number;
            PRINT_DEBUG("Set descendant signal to %d (%s)", descendant_signal_to_send, signal_names[i].name);
			return 0;
		}
	}
	char *endptr = NULL; long sig_num = strtol(arg, &endptr, 10);
    if (endptr != NULL && *endptr == '\0' && sig_num > 0 && sig_num < NSIG) {
        descendant_signal_to_send = (int)sig_num;
        PRINT_DEBUG("Parsed descendant signal number: %d", descendant_signal_to_send);
        return 0;
    }
	return 1; // Signal name/number not valid
}

int parse_args(const int argc, char* const argv[], char* (**child_args_ptr_ptr)[], int* const parse_fail_exitcode_ptr) {
	char* name = argv[0];
	if (argc == 2 && strcmp("--version", argv[1]) == 0) {
		*parse_fail_exitcode_ptr = 0; fprintf(stdout, "%s\n", TINI_VERSION_STRING); return 1;
	}
#ifndef TINI_MINIMAL
	int c;
	while ((c = getopt(argc, argv, "+" OPT_STRING)) != -1) {
		switch (c) {
			case 'h': print_usage(name, stdout); *parse_fail_exitcode_ptr = 0; return 1;
#if HAS_SUBREAPER
			case 's': subreaper++; break;
#endif
			case 'p': if (set_pdeathsig(optarg)) { PRINT_FATAL("Not a valid option for -p: %s", optarg); *parse_fail_exitcode_ptr = 1; return 1; } break;
			case 'v': verbosity++; break;
			case 'w': warn_on_reap++; break;
			case 'g': kill_process_group++; break;
			case 'e': if (add_expect_status(optarg)) { PRINT_FATAL("Not a valid option for -e: %s", optarg); *parse_fail_exitcode_ptr = 1; return 1; } break;
            case 'a': kill_descendants_on_exit++; break;
            case 'k': if (set_descendant_signal(optarg)) { PRINT_FATAL("Not a valid signal name or number for -k: %s", optarg); print_usage(name, stderr); *parse_fail_exitcode_ptr = 1; return 1; } break;
			case 'l': print_license(stdout); *parse_fail_exitcode_ptr = 0; return 1;
			case '?': print_usage(name, stderr); *parse_fail_exitcode_ptr = 1; return 1;
			default: *parse_fail_exitcode_ptr = 1; return 1; // Should not happen
		}
	}
#endif // TINI_MINIMAL
	*child_args_ptr_ptr = calloc(argc-optind+1, sizeof(char*));
	if (*child_args_ptr_ptr == NULL) {
		PRINT_FATAL("Failed to allocate memory for child args: '%s'", strerror(errno)); *parse_fail_exitcode_ptr = 1; return 1;
	}
	int i;
	for (i = 0; i < argc - optind; i++) { (**child_args_ptr_ptr)[i] = argv[optind+i]; }
	(**child_args_ptr_ptr)[i] = NULL;
	if (i == 0) { print_usage(name, stderr); *parse_fail_exitcode_ptr = 1; free(*child_args_ptr_ptr); return 1; } // Free allocated memory before returning
	return 0; // Success
}

int parse_env(void) {
#if HAS_SUBREAPER
	if (getenv(SUBREAPER_ENV_VAR) != NULL) { subreaper++; }
#endif
	if (getenv(KILL_PROCESS_GROUP_GROUP_ENV_VAR) != NULL) { kill_process_group++; }
	char* env_verbosity = getenv(VERBOSITY_ENV_VAR);
	if (env_verbosity != NULL) {
		long val = strtol(env_verbosity, NULL, 10);
		if (val >= 0 && val <= 4) { verbosity = (unsigned int)val; }
        else { PRINT_WARNING("Invalid value for %s: %s. Using default.", VERBOSITY_ENV_VAR, env_verbosity); }
	}
	return 0;
}

#if HAS_SUBREAPER
int register_subreaper (void) {
	if (subreaper > 0) {
		if (prctl(PR_SET_CHILD_SUBREAPER, 1)) {
			if (errno == EINVAL) { PRINT_FATAL("PR_SET_CHILD_SUBREAPER is unavailable on this platform. Are you using Linux >= 3.4?"); }
            else { PRINT_FATAL("Failed to register as child subreaper: %s", strerror(errno)); }
			return 1;
		} else { PRINT_TRACE("Registered as child subreaper"); }
	}
	return 0;
}
#endif

void reaper_check (void) {
#if HAS_SUBREAPER
	int bit = 0;
#endif
	if (getpid() == 1) { return; }
#if HAS_SUBREAPER
	if (prctl(PR_GET_CHILD_SUBREAPER, &bit)) { PRINT_DEBUG("Failed to read child subreaper attribute: %s", strerror(errno)); }
    else if (bit == 1) { return; }
#endif
	PRINT_WARNING(reaper_warning);
}

int configure_signals(sigset_t* const parent_sigset_ptr, const signal_configuration_t* const sigconf_ptr) {
	if (sigfillset(parent_sigset_ptr)) { PRINT_FATAL("sigfillset failed: '%s'", strerror(errno)); return 1; }
	int signals_for_tini[] = {SIGFPE, SIGILL, SIGSEGV, SIGBUS, SIGABRT, SIGTRAP, SIGSYS, SIGTTIN, SIGTTOU};
	for (size_t i = 0; i < ARRAY_LEN(signals_for_tini); i++) { // Use size_t for loop
		if (sigdelset(parent_sigset_ptr, signals_for_tini[i])) { PRINT_FATAL("sigdelset failed: '%i'", signals_for_tini[i]); return 1; }
	}
	if (sigprocmask(SIG_SETMASK, parent_sigset_ptr, sigconf_ptr->sigmask_ptr)) { PRINT_FATAL("sigprocmask failed: '%s'", strerror(errno)); return 1; }
	struct sigaction ign_action; memset(&ign_action, 0, sizeof ign_action);
	ign_action.sa_handler = SIG_IGN; sigemptyset(&ign_action.sa_mask);
	if (sigaction(SIGTTIN, &ign_action, sigconf_ptr->sigttin_action_ptr)) { PRINT_FATAL("Failed to ignore SIGTTIN"); return 1; }
	if (sigaction(SIGTTOU, &ign_action, sigconf_ptr->sigttou_action_ptr)) { PRINT_FATAL("Failed to ignore SIGTTOU"); return 1; }
	return 0;
}

int wait_and_forward_signal(sigset_t const* const parent_sigset_ptr, pid_t const child_pid) {
	siginfo_t sig;
	if (sigtimedwait(parent_sigset_ptr, &sig, &ts) == -1) {
		switch (errno) {
			case EAGAIN: break; // Timeout is expected
			case EINTR: PRINT_DEBUG("sigtimedwait received EINTR"); break; // Should be rare with our mask
			default: PRINT_FATAL("Unexpected error in sigtimedwait: '%s'", strerror(errno)); return 1;
		}
        return 0; // Continue loop on timeout or EINTR
	} else {
		PRINT_DEBUG("Received signal %d (%s)", sig.si_signo, strsignal(sig.si_signo));
		if (sig.si_signo == SIGCHLD) {
			PRINT_DEBUG("Received SIGCHLD"); // Handled by reap_zombies call in main loop
		} else {
			PRINT_DEBUG("Passing signal %d (%s) to child %d (or group)", sig.si_signo, strsignal(sig.si_signo), child_pid);
			pid_t target_pid = kill_process_group ? -child_pid : child_pid;
			if (kill(target_pid, sig.si_signo)) {
				if (errno == ESRCH) { PRINT_WARNING("Child process (pid: %d%s) was already dead when forwarding signal %d.", child_pid, kill_process_group ? " group" : "", sig.si_signo); }
                else { PRINT_FATAL("Unexpected error when forwarding signal %d: '%s'", sig.si_signo, strerror(errno)); return 1; }
			}
		}
        return 0; // Signal handled or is SIGCHLD
	}
}


int reap_zombies(const pid_t child_pid, int* const child_exitcode_ptr) {
	pid_t current_pid; int current_status;
	while (1) {
		current_pid = waitpid(-1, &current_status, WNOHANG);
		if (current_pid < 0) {
            if (errno == ECHILD) { PRINT_TRACE("No more children to wait for."); break; }
            else { PRINT_FATAL("Error while waiting for pids: '%s'", strerror(errno)); return 1; }
        } else if (current_pid == 0) {
            PRINT_TRACE("No child to reap right now."); break;
        } else {
            PRINT_DEBUG("Reaped process with pid: '%i'", current_pid);
            if (current_pid == child_pid) {
                if (*child_exitcode_ptr != -1) {
                     PRINT_WARNING("Received SIGCHLD for main child (pid: %d) which already exited.", child_pid);
                } else {
                    if (WIFEXITED(current_status)) {
                        PRINT_INFO("Main child exited normally (pid: %d, status: %i)", child_pid, WEXITSTATUS(current_status)); *child_exitcode_ptr = WEXITSTATUS(current_status);
                    } else if (WIFSIGNALED(current_status)) {
                        PRINT_INFO("Main child exited with signal (pid: %d, signal: %s)", child_pid, strsignal(WTERMSIG(current_status))); *child_exitcode_ptr = 128 + WTERMSIG(current_status);
                    } else {
                        PRINT_FATAL("Main child (pid: %d) changed state unexpectedly (status: %d)", child_pid, current_status); return 1;
                    }
                    *child_exitcode_ptr = *child_exitcode_ptr % (STATUS_MAX - STATUS_MIN + 1);
                    INT32_BITFIELD_CHECK_BOUNDS(expect_status, *child_exitcode_ptr);
                    if (INT32_BITFIELD_TEST(expect_status, *child_exitcode_ptr)) {
                        PRINT_INFO("Remapping main child exit code %d to 0", *child_exitcode_ptr); *child_exitcode_ptr = 0;
                    }
                 }
            } else {
                 if (warn_on_reap > 0) { PRINT_WARNING("Reaped zombie process with pid=%i", current_pid); }
                 else { PRINT_DEBUG("Reaped descendant or adopted process pid=%i", current_pid); }
            }
            continue; // Check for more zombies immediately
        }
	}
	return 0; // Reaping cycle finished successfully
}

int kill_descendants(pid_t parent_pid_of_descendants) {
    PRINT_INFO("Killing descendant processes of pid %d with signal %d (%s)...",
               parent_pid_of_descendants, descendant_signal_to_send, strsignal(descendant_signal_to_send));

    DescendantList descendants = get_process_descendants(parent_pid_of_descendants);
    int success = 0; // Assume success unless kill fails non-ESRCH

    if (descendants.pids == NULL && descendants.count == 0 && errno != 0) {
        PRINT_DEBUG("No descendants found or error retrieving list for pid %d.", parent_pid_of_descendants);
    } else if (descendants.count > 0) {
        PRINT_INFO("Found %zu descendant(s) to signal.", descendants.count);
        for (size_t i = 0; i < descendants.count; ++i) {
            pid_t desc_pid = descendants.pids[i];
            PRINT_DEBUG("Sending signal %d to descendant PID %d", descendant_signal_to_send, desc_pid);

            if (kill(desc_pid, descendant_signal_to_send) == -1) {
                if (errno == ESRCH) {
                    PRINT_DEBUG("Descendant PID %d already exited.", desc_pid);
                } else {
                    PRINT_WARNING("Failed to send signal %d to descendant PID %d: %s", descendant_signal_to_send, desc_pid, strerror(errno));
                    success = -1; // Mark as failed if any kill fails unexpectedly
                }
            }
        }
    } else {
        PRINT_INFO("No descendants found for pid %d.", parent_pid_of_descendants);
    }

    free_descendant_list(&descendants);

    return success == 0 ? 0 : 1; // Return 0 on success, 1 on unexpected kill error
}

int main(int argc, char *argv[]) {
	pid_t tini_pid = getpid();
	pid_t child_pid = -1;
	int child_exitcode = -1;
	int parse_exitcode = 1;

	char* (*child_args_ptr)[];
	if (parse_args(argc, argv, &child_args_ptr, &parse_exitcode)) {
		return parse_exitcode;
	}
	if (parse_env()) { free(*child_args_ptr); return 1; }

	sigset_t parent_sigset, child_sigset;
	struct sigaction sigttin_action, sigttou_action;
	memset(&sigttin_action, 0, sizeof sigttin_action); memset(&sigttou_action, 0, sizeof sigttou_action);
	signal_configuration_t child_sigconf = { &child_sigset, &sigttin_action, &sigttou_action };
	if (configure_signals(&parent_sigset, &child_sigconf)) { free(*child_args_ptr); return 1; }

	if (parent_death_signal && prctl(PR_SET_PDEATHSIG, parent_death_signal)) {
		PRINT_FATAL("Failed to set up parent death signal (%d): %s", parent_death_signal, strerror(errno));
        free(*child_args_ptr); return 1;
	 }
#if HAS_SUBREAPER
	if (register_subreaper()) { free(*child_args_ptr); return 1; };
#endif
	reaper_check();

	int spawn_ret = spawn(&child_sigconf, *child_args_ptr, &child_pid);
    free(*child_args_ptr); child_args_ptr = NULL; // Free args immediately after use
	if (spawn_ret) { return spawn_ret; }

	while (1) {
		if (wait_and_forward_signal(&parent_sigset, child_pid)) { return 1; }
		if (reap_zombies(child_pid, &child_exitcode)) { return 1; }

		if (child_exitcode != -1) {
			PRINT_TRACE("Main child (pid: %d) has exited with code %d.", child_pid, child_exitcode);
            if (kill_descendants_on_exit) {
                if (kill_descendants(tini_pid)) {
                    PRINT_WARNING("Failed to kill one or more descendants cleanly.");
                }
                // Optional final reap
                int dummy_exit_code = child_exitcode;
                if (reap_zombies(child_pid, &dummy_exit_code)){
                    PRINT_WARNING("Error during final reaping after killing descendants.");
                }
            }
			PRINT_TRACE("Exiting with final code: %d", child_exitcode);
			return child_exitcode;
		}
        PRINT_TRACE("Main loop continuing (child pid: %d still running)", child_pid);
	}
    // Should not be reached
    return 255;
}
