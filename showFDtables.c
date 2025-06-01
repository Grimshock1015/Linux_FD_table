#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>

typedef struct {
    int per_process;
    int system_wide;
    int vnodes;
    int composite;
    int summary;
    int threshold;
    pid_t target_pid;
    int output_txt;
    int output_bin;
} Arguments;

// Structure to store FD information for a process
typedef struct FDNode {
    pid_t pid;
    int fd;
    char target[256];
    ino_t inode;
    struct FDNode *next;
} FDNode;

// Structure to store FD summary per process
typedef struct FDSummary {
    pid_t pid;
    int fd_count;
    struct FDSummary *next;
} FDSummary;

FDNode* sortedMerge(FDNode* a, FDNode* b) {
    if (!a) return b;
    if (!b) return a;

    FDNode* result = NULL;

    if (a->pid < b->pid || (a->pid == b->pid && a->fd < b->fd)) {
        result = a;
        result->next = sortedMerge(a->next, b);
    } else {
        result = b;
        result->next = sortedMerge(a, b->next);
    }
    return result;
}

void splitList(FDNode* source, FDNode** front, FDNode** back) {
    if (!source || !source->next) {
        *front = source;
        *back = NULL;
        return;
    }

    FDNode* slow = source;
    FDNode* fast = source->next;

    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
    }

    *front = source;
    *back = slow->next;
    slow->next = NULL;
}

FDNode* mergeSort(FDNode* head) {
    if (!head || !head->next) return head;

    FDNode* front;
    FDNode* back;

    splitList(head, &front, &back);

    front = mergeSort(front);
    back = mergeSort(back);

    return sortedMerge(front, back);
}

/**
 * Merge function for merge sort
 */
FDSummary* sortedMergeSum(FDSummary* a, FDSummary* b) {
    if (!a) return b;
    if (!b) return a;
    
    FDSummary* result = NULL;
    
    if (a->pid <= b->pid) {
        result = a;
        result->next = sortedMergeSum(a->next, b);
    } else {
        result = b;
        result->next = sortedMergeSum(a, b->next);
    }
    return result;
}

/**
 * Function to split a linked list into two halves
 */
void splitListSum(FDSummary* source, FDSummary** front, FDSummary** back) {
    if (!source || !source->next) {
        *front = source;
        *back = NULL;
        return;
    }
    
    FDSummary* slow = source;
    FDSummary* fast = source->next;
    
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
    }
    
    *front = source;
    *back = slow->next;
    slow->next = NULL;
}

/**
 * Merge sort function for FDSummary list
 */
FDSummary* mergeSortSummary(FDSummary* head) {
    if (!head || !head->next) return head;
    
    FDSummary* front;
    FDSummary* back;
    
    splitListSum(head, &front, &back);
    
    front = mergeSortSummary(front);
    back = mergeSortSummary(back);
    
    return sortedMergeSum(front, back);
}

/**
 * Function to get uid of a given process
 */
uid_t get_process_uid(pid_t pid){
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);

    FILE* file = fopen(path, "r");
    if (!file){
        return (uid_t)-1;  // Unable to read UID
    } 
    uid_t uid = (uid_t)-1;
    char line[256];

    while (fgets(line, sizeof(line), file)){
        if (strncmp(line, "Uid:", 4) == 0){ // Checks if Uid
            sscanf(line, "Uid: %u", &uid);  // Copies Uid
            break;
        }

    }
    fclose(file);
    return uid;
}

/**
 * Function to collect initialise a FDnode and insert to a list
 */
void add_fd(FDNode** fd_list, pid_t pid, int fd, char * target, ino_t inode){
    FDNode *new_node = malloc(sizeof(FDNode));
    if (!new_node) {
        perror("Failed to allocate memory for FDNode");
        return;
    }

    new_node->pid = pid;
    new_node->fd = fd;
    strncpy(new_node->target, target, sizeof(new_node->target));
    new_node->inode = inode;
    new_node->next = *fd_list;
    *fd_list = new_node;

}

/**
 * Function to read fd_list and update info in sum_list
 */
void update_fd_summary(FDNode *fd_list, FDSummary ** sum_list){
    while (fd_list){
        FDSummary *current = *sum_list;
        FDSummary *prev = NULL;
        while (current){
            if (current->pid == fd_list->pid){
                current->fd_count++;
                break;
            }
            prev = current;
            current = current->next;
        }
        if (!current) {
            FDSummary *new_summary = malloc(sizeof(FDSummary));
            if (!new_summary) {
                perror("Failed to allocate memory for summary node");  
                // Handle memory allocation failure
                exit(EXIT_FAILURE);
                return;
            }
            new_summary->pid = fd_list->pid;
            new_summary->fd_count = 1;
            new_summary->next = NULL;
            
            if (prev) {
                prev->next = new_summary;
            } 
            else { //First element
                *sum_list = new_summary;
            }
        }
        fd_list = fd_list->next;
    }
}

/**
 * Function to collect inode of a given process
 */
ino_t get_inode(pid_t pid, int fd){
    char fdinfo_path[256];
    snprintf(fdinfo_path, sizeof(fdinfo_path), "/proc/%d/fdinfo/%d", pid, fd);
    FILE *file = fopen(fdinfo_path, "r");
    if (!file) return -1;  // Unable to read file

    ino_t inode = 0;
    char line[256];
    
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "ino:", 4) == 0) { // Checks if inode
            sscanf(line, "ino: %lu", &inode); // Copies inode
            break;
        }
    }
    fclose(file);
    return inode;
}

/**
 * Function to collect FD data (FD number, filename, and inode)
 */
void collect_FD(FDNode** fd_list, int target_pid) {
    uid_t user_uid = getuid(); // Obtains user Uid
    DIR *proc = opendir("/proc");
    if (!proc) return;
    struct dirent *entry;
    while ((entry = readdir(proc)) != NULL){
        if(!isdigit(entry->d_name[0])) {
            continue; // Continue if not process
        }
        pid_t pid = atoi(entry->d_name);
        if (target_pid == 0 && get_process_uid(pid) != user_uid) {
            //Didnt specify, uid != user uid
            continue;
        }
        else if (target_pid != 0 && pid != target_pid){
            continue;
        }
        char fd_path[265];
        snprintf(fd_path, sizeof(fd_path), "/proc/%d/fd", pid);
        // Gets /proc/pid/fd

        DIR *fd_dir = opendir(fd_path);
        if (!fd_dir) continue;

        struct dirent *fd_entry;
        while ((fd_entry = readdir(fd_dir)) != NULL) {
            if (fd_entry->d_name[0] == '.'){
                continue; // Skips misc
            }
            int fd = atoi(fd_entry->d_name);
            char link_path[512], target[512];
            int written = snprintf(link_path, sizeof(link_path), "%s/%d", fd_path, fd);
            // To read files/get file path
            // Check if snprintf truncated the output
            if (written < 0 || (size_t)written >= sizeof(link_path)) {
                fprintf(stderr, "Warning: Truncated output in snprintf for FD %d\n", fd);
                continue;  // Handle the error properly (e.g., skipping this entry)
            }
            ssize_t len = readlink(link_path, target, sizeof(target) - 1);

            if (len != -1) {
                target[len] = '\0';  // Null-terminate
                ino_t inode = get_inode(pid, fd);
                add_fd(fd_list, pid, fd, target, inode);
            }
            if (len == -1) {
                perror("readlink failed");
                continue;
            }
        }
        closedir(fd_dir);
    }
    closedir(proc);
}

/**
 * Function to save composite table in text format
 */
void save_composite_table_txt(FDNode *fd_list) {
    FILE *file = fopen("compositeTable.txt", "w");
    if (!file) {
        perror("Failed to open compositeTable.txt");
        return;
    }
    
    fprintf(file, "\n%-10s %-5s %-25s %-10s \n", "PID", "FD", "Filename", "Inode");
    fprintf(file, "=============================================\n");
    FDNode *node = fd_list;
    while (node) {
        fprintf(file, "%-10d %-5d %-25s %-10ju\n", node->pid, node->fd, node->target, (uintmax_t)node->inode);
        node = node->next;
    }
    fprintf(file, "=============================================\n");
    
    fclose(file);
}

/**
 * Function to save composite table in binary format
 */
void save_composite_table_bin(FDNode *fd_list) {
    FILE *file = fopen("compositeTable.bin", "wb");
    if (!file) {
        perror("Failed to open compositeTable.bin");
        return;
    }
    
    FDNode *node = fd_list;
    while (node) {
        fwrite(node, sizeof(FDNode), 1, file);
        node = node->next;
    }
    fclose(file);
}

/**
 * Parse command-line arguments and update the Arguments struct.
 */
void parse_arguments(int argc, char *argv[], Arguments *args) {
    args->per_process = 0;
    args->system_wide = 0;
    args->vnodes = 0;
    args->composite = 0;
    args->summary = 0;
    args->threshold = 0; // Default: No threshold
    args->target_pid = 0; // Default: Process all PIDs
    args->output_bin = 0;
    args->output_txt = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--per-process") == 0) {
            args->per_process = 1;
        } 
        else if (strcmp(argv[i], "--systemWide") == 0) {
            args->system_wide = 1;
        } 
        else if (strcmp(argv[i], "--Vnodes") == 0) {
            args->vnodes = 1;
        } 
        else if (strcmp(argv[i], "--composite") == 0) {
            args->composite = 1;
        } 
        else if (strcmp(argv[i], "--output_TXT") == 0) {
            args->output_txt = 1;
        }
        else if (strcmp(argv[i], "--output_binary") == 0) {
            args->output_bin = 1;
        }
        else if (strcmp(argv[i], "--summary") == 0) {
            args->summary = 1;
        } 
        else if (strncmp(argv[i], "--threshold=", 12) == 0) {
            int value = atoi(argv[i] + 12);
            if (value < 0) {
                fprintf(stderr, "Error: --threshold cannot be negative (%d).\n", value);
                exit(EXIT_FAILURE);
            }
            args->threshold = value;
        } 
        else if (strspn(argv[i], "0123456789") == strlen(argv[i])) {
            // If it's a number, assume it's a positional argument (PID)
            args->target_pid = atoi(argv[i]);
        }
        else{
            fprintf(stderr, "Error: Unknown flag '%s'\n", argv[i]);
            exit(EXIT_FAILURE);
        }
    }

    // If no flags are given, default to displaying all tables
    if (!args->per_process && !args->system_wide && !args->vnodes && !args->composite && !args->summary) {
        args->composite = 1;
    }
}

/**
 * Function to display FD table
 */
void display_per_process_table(FDNode *fd_list) {
    printf("\n%-10s %-5s\n", "PID", "FD");
    printf("===============\n");

    FDNode *node = fd_list;
    while (node) {
        printf("%-10d %-5d\n", node->pid, node->fd);
        node = node->next;
    }
    printf("===============\n");
}

/**
 * Function to display systemwide FD table
 */
void display_systemwide_table(FDNode *fd_list) {
    printf("\n%-10s %-5s %-10s\n", "PID", "FD", "Filename");
    printf("=========================\n");

    FDNode *node = fd_list;
    while (node) {
        printf("%-10d %-5d %-10s\n", node->pid, node->fd, node->target);
        node = node->next;
    }
    printf("=========================\n");
    
}

/**
 * Function to display inode FD table
 */
void display_inode_table(FDNode *fd_list) {
    printf("\n%-5s %-10s\n", "FD", "Inode");
    printf("===============\n");

    FDNode *node = fd_list;
    while (node) {
        printf("%-5d %-10ju\n", node->fd, (uintmax_t)node->inode);
        node = node->next;
    }
    printf("===============\n");
    
}

/**
 * Function to display composite FD table
 */
void display_composite_table(FDNode *fd_list) {
    printf("\n%-10s %-5s %-25s %-10s \n", "PID", "FD", "Filename", "Inode");
    printf("=============================================\n");

    FDNode *node = fd_list;
    while (node) {
        printf("%-10d %-5d %-25s %-10ju\n", node->pid, node->fd, node->target, (uintmax_t)node->inode);
        node = node->next;
    }
    printf("=============================================\n");
    
}


/**
 * Function to display summary table
 */
void display_summary_table(FDSummary *fd_sum) {
    
    printf("Summary Table\n");
    printf("===============\n");

    FDSummary *node = fd_sum;
    while (node) {
        printf("%d (%d), ", node->pid, node->fd_count);
        node = node->next;
    }
    printf("\n");
    printf("===============\n");    
}

void clearScreen(){
    printf("\033[H\033[J");
}

/**
 * Function to display threshold table
 */
void display_threshold_table(FDNode *fd_list, FDSummary *fd_sum, int threshold) {
    printf("## Offending processes:\n");
    FDSummary *node = fd_sum;
    while (node) {
        if (node->fd_count > threshold){
            printf("%d (%d), ", node->pid, node->fd_count);
        }
        node = node->next;
    }
    printf("\n"); 
}
/**
 * Function to free the FD linked list
 */
void free_fd_list(FDNode *head) {
    FDNode *temp;
    while (head) {
        temp = head;
        head = head->next;
        free(temp);
    }
}

/**
 * Function to free the FDSummary linked list
 */
void free_fd_summary(FDSummary *head) {
    FDSummary *temp;
    while (head) {
        temp = head;
        head = head->next;
        free(temp);
    }
}
/**
 * Function to output the respective requirements from argument
 */
void display_tables(Arguments* argument){
    clearScreen();
    FDNode *fd_list = NULL;
    collect_FD(&fd_list, argument->target_pid);
    fd_list = mergeSort(fd_list);
    FDSummary *fd_summary = NULL;
    update_fd_summary(fd_list, &fd_summary);
    fd_summary = mergeSortSummary(fd_summary);
    if (argument->per_process == 1){
        display_per_process_table(fd_list);
    }
    if (argument->system_wide == 1){
        display_systemwide_table(fd_list);
    }
    if (argument->vnodes == 1){
        display_inode_table(fd_list);
    }
    if (argument->composite == 1){
        display_composite_table(fd_list);
    }
    if (argument->summary == 1){
        display_summary_table(fd_summary);
    }
    if (argument->threshold > 0){
        display_threshold_table(fd_list, fd_summary, argument->threshold);
    }
    if (argument->output_bin != 0){
        save_composite_table_bin(fd_list);
    }
    if (argument->output_txt != 0){
        save_composite_table_txt(fd_list);
    }
    free_fd_list(fd_list);
    free_fd_summary(fd_summary);
}

int main(int argc, char **argv) {
    Arguments arguments;
    memset(&arguments, 0, sizeof(Arguments));
    parse_arguments(argc, argv, &arguments);
    display_tables(&arguments);
    return 0;
}