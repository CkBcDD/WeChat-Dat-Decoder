#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#ifdef _WIN32
    #include <windows.h>
    #include <process.h>  // For _beginthreadex
    #define PATH_SEPARATOR "\\"
    #define SAFE_STRNCPY(dest, size, src) strncpy_s(dest, size, src, _TRUNCATE)
    #define IS_DIR(mode) ((mode) & _S_IFDIR)
    #define IS_REG(mode) ((mode) & _S_IFREG)
    #define SAFE_STRNCAT(dest, size, src) strcat_s(dest, size, src)
    typedef HANDLE thread_t;
    #define THREAD_FUNC_RETURN unsigned int __stdcall
#else
    #include <dirent.h>
    #include <unistd.h>
    #include <pthread.h>
    #define PATH_SEPARATOR "/"
    #define SAFE_STRNCPY(dest, size, src) strncpy(dest, src, (size) - 1); dest[(size) - 1] = '\0'
    #define IS_DIR(mode) S_ISDIR(mode)
    #define IS_REG(mode) S_ISREG(mode)
    #define SAFE_STRNCAT(dest, size, src) strncat(dest, src, (size) - strlen(dest) - 1)
    typedef pthread_t thread_t;
    #define THREAD_FUNC_RETURN void *
#endif

#define DECODING_SUFFIX "decoded"
#define DECODING_SUFFIX_LEN 7

int flag = 0;
uint64_t coder = 0;
int silent_mode = 0;

#ifdef __SSE2__
    #include <emmintrin.h>
#endif

// Determine the coder value based on the first two bytes of the file
static inline int check_coder(unsigned char a, unsigned char b) {
    unsigned char x = a ^ b;
    switch (x) {
        case 0x27:  // JPEG
            if ((a ^ 0xFF) == (b ^ 0xD8))
                return a ^ 0xFF;
            break;
        case 0xD9:  // PNG
            if ((a ^ 0x89) == (b ^ 0x50))
                return a ^ 0x89;
            break;
        case 0x0F:  // BMP
            if ((a ^ 0x42) == (b ^ 0x4D))
                return a ^ 0x42;
            break;
        case 0x0E:  // GIF
            if ((a ^ 0x47) == (b ^ 0x49))
                return a ^ 0x47;
            break;
        case 0x1B:  // ZIP
            if ((a ^ 0x50) == (b ^ 0x4B))
                return a ^ 0x50;
            break;
        case 0x33:  // RAR
            if ((a ^ 0x52) == (b ^ 0x61))
                return a ^ 0x52;
            break;
        case 0x17:  // AVI
            if ((a ^ 0x41) == (b ^ 0x56))
                return a ^ 0x41;
            break;
        default:
            break;
    }
    return 0;
}

// Determine the file extension based on the file header data
static inline const char *get_extension(unsigned char *data) {
    switch (data[0]) {
        case 0xFF:
            if (data[1] == 0xD8 && data[2] == 0xFF)
                return ".jpg";
            break;
        case 0x89:
            if (data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47)
                return ".png";
            break;
        case 0x42:
            if (data[1] == 0x4D)
                return ".bmp";
            break;
        case 0x47:
            if (data[1] == 0x49 && data[2] == 0x46)
                return ".gif";
            break;
        case 0x25:
            if (data[1] == 0x50 && data[2] == 0x44 && data[3] == 0x46)
                return ".pdf";
            break;
        case 0x50:
            if (data[1] == 0x4B)
                return ".zip";
            break;
        case 0x52:
            if (data[1] == 0x49 && data[2] == 0x46 && data[3] == 0x46)
                return ".avi";
            break;
        default:
            break;
    }
    return ".unknown";
}

// Construct the output directory path where decoded files will be stored
static inline void construct_output_dir(const char *dir_path, char *output_dir, size_t size) {
    size_t dir_path_len = strlen(dir_path);
    if (dir_path_len + strlen(PATH_SEPARATOR) + DECODING_SUFFIX_LEN + 1 < size) {
        snprintf(output_dir, size, "%s%s%s", dir_path, PATH_SEPARATOR, DECODING_SUFFIX);
    }
    else {
        if (!silent_mode) printf("Output directory path too long: %s%s%s\n", dir_path, PATH_SEPARATOR, DECODING_SUFFIX);
        output_dir[0] = '\0';
    }
}

// Extract the base filename from the filepath, excluding the extension
static inline void extract_filename(const char *filepath, char *filename, size_t size) {
    const char *base_name = strrchr(filepath, '\\');
    if (!base_name) {
        base_name = strrchr(filepath, '/');
    }
    if (!base_name) {
        base_name = filepath;
    } else {
        base_name++;
    }

    SAFE_STRNCPY(filename, size, base_name);
    char *dot = strrchr(filename, '.');
    if (dot && strcmp(dot, ".dat") == 0) {
        *dot = '\0';
    }
}

// Construct the full output file path for the decoded file
static inline int construct_output_path(const char *output_dir, const char *filename, const char *ext, char *output_path, size_t size) {
    int ret = snprintf(output_path, size, "%s%s%s", output_dir, PATH_SEPARATOR, filename);
    if (ret < 0 || (size_t)ret >= size) {
        if (!silent_mode) printf("Output path too long: %s%s%s\n", output_dir, PATH_SEPARATOR, filename);
        return -1;
    }

    if (strlen(output_path) + strlen(ext) + 1 < size) {
        SAFE_STRNCAT(output_path, size, ext);
    } else {
        if (!silent_mode) printf("Output path with extension too long: %s%s\n", output_path, ext);
        return -1;
    }

    return 0;
}

// Structure to hold arguments for the thread function
typedef struct {
    char filepath[1024];
    char output_dir[1024];
} thread_arg_t;

// Thread function to process a single file
THREAD_FUNC_RETURN thread_process_file(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    const char *filepath = targ->filepath;
    const char *output_dir = targ->output_dir;

    // Open the input file for reading
    FILE *p = fopen(filepath, "rb");
    if (!p) {
        if (!silent_mode) printf("Unable to open file: %s\n", filepath);
        free(targ);
        #ifdef _WIN32
            return 0;
        #else
            return NULL;
        #endif
    }

    // Get the size of the file
    fseek(p, 0, SEEK_END);
    long length = ftell(p);
    fseek(p, 0, SEEK_SET);

    if (length <= 0) {
        if (!silent_mode) printf("File is empty or unreadable: %s\n", filepath);
        fclose(p);
        free(targ);
        #ifdef _WIN32
            return 0;
        #else
            return NULL;
        #endif
    }

    // Allocate memory for the file buffer with alignment for SIMD operations
    unsigned char *buf;
    #ifdef _WIN32
        buf = (unsigned char *)_aligned_malloc(length, 16);
    #else
        if (posix_memalign((void **)&buf, 16, length) != 0) {
            buf = NULL;
        }
    #endif

    if (!buf) {
        if (!silent_mode) printf("Memory allocation failed for file: %s\n", filepath);
        fclose(p);
        free(targ);
        #ifdef _WIN32
            return 0;
        #else
            return NULL;
        #endif
    }

    // Read the file data into the buffer
    size_t read_length = fread(buf, 1, length, p);
    fclose(p);
    if (read_length != (size_t)length) {
        if (!silent_mode) printf("File read failed: %s\n", filepath);
        #ifdef _WIN32
            _aligned_free(buf);
        #else
            free(buf);
        #endif
        free(targ);
        #ifdef _WIN32
            return 0;
        #else
            return NULL;
        #endif
    }

    // Initialize coder value if it has not been set yet
    if (!flag) {
        coder = check_coder(buf[0], buf[1]);
        if (coder == 0) {
            if (!silent_mode) printf("Unable to determine coder value: %s\n", filepath);
            #ifdef _WIN32
                _aligned_free(buf);
            #else
                free(buf);
            #endif
            free(targ);
            #ifdef _WIN32
                return 0;
            #else
                return NULL;
            #endif
        }
        flag = 1;
    }

    // Decrypt the file data using SIMD if supported, otherwise use a standard loop
    #ifdef __SSE2__
    {
        __m128i coder_vec = _mm_set1_epi8((char)coder);
        long i;
        for (i = 0; i + 15 < length; i += 16) {
            __m128i data = _mm_load_si128((__m128i *)(buf + i));
            data = _mm_xor_si128(data, coder_vec);
            _mm_store_si128((__m128i *)(buf + i), data);
        }
        for (; i < length; i++) {
            buf[i] ^= (unsigned char)coder;
        }
    }
    #else
    {
        for (long i = 0; i < length; i++) {
            buf[i] ^= (unsigned char)coder;
        }
    }
    #endif

    // Determine the file extension based on the decrypted data
    const char *ext = get_extension(buf);

    // Create the output directory if it does not exist
    #ifdef _WIN32
        CreateDirectoryA(output_dir, NULL);
    #else
        mkdir(output_dir, 0755);
    #endif

    // Extract the base filename from the filepath
    char filename[256];
    extract_filename(filepath, filename, sizeof(filename));

    // Construct the full output file path for the decoded file
    char output_path[1024];
    if (construct_output_path(output_dir, filename, ext, output_path, sizeof(output_path)) != 0) {
        #ifdef _WIN32
            _aligned_free(buf);
        #else
            free(buf);
        #endif
        free(targ);
        #ifdef _WIN32
            return 0;
        #else
            return NULL;
        #endif
    }

    // Open the output file for writing the decoded data
    FILE *o = fopen(output_path, "wb");
    if (!o) {
        if (!silent_mode) printf("Unable to create file: %s\n", output_path);
        #ifdef _WIN32
            _aligned_free(buf);
        #else
            free(buf);
        #endif
        free(targ);
        #ifdef _WIN32
            return 0;
        #else
            return NULL;
        #endif
    }

    // Write the decoded data to the output file
    fwrite(buf, length, 1, o);
    fclose(o);

    if (!silent_mode) printf("Decoded file: %s\n", output_path);

    // Free allocated resources
    #ifdef _WIN32
        _aligned_free(buf);
    #else
        free(buf);
    #endif
    free(targ);

    #ifdef _WIN32
        return 0;
    #else
        return NULL;
    #endif
}

// Process all .dat files in the given directory
void process_directory(const char *dir_path) {
    char output_dir[1024];
    construct_output_dir(dir_path, output_dir, sizeof(output_dir));
    if (output_dir[0] == '\0') return;

    #ifdef _WIN32
        WIN32_FIND_DATAA fd;
        HANDLE hFind = INVALID_HANDLE_VALUE;

        // Search for .dat files in the directory
        char search_path[1024];
        snprintf(search_path, sizeof(search_path), "%s\\*.dat", dir_path);

        hFind = FindFirstFileA(search_path, &fd);
        if (hFind == INVALID_HANDLE_VALUE) {
            if (!silent_mode) printf("No .dat files found in directory: %s\n", dir_path);
            return;
        }

        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                char file_path[1024];
                snprintf(file_path, sizeof(file_path), "%s\\%s", dir_path, fd.cFileName);

                // Allocate memory for thread arguments
                thread_arg_t *targ = malloc(sizeof(thread_arg_t));
                if (!targ) {
                    if (!silent_mode) printf("Memory allocation failed for thread arguments.\n");
                    continue;
                }
                strncpy_s(targ->filepath, sizeof(targ->filepath), file_path, _TRUNCATE);
                strncpy_s(targ->output_dir, sizeof(targ->output_dir), output_dir, _TRUNCATE);

                // Create a thread to process the file
                HANDLE thread;
                thread = (HANDLE)_beginthreadex(NULL, 0, thread_process_file, targ, 0, NULL);
                if (thread == NULL) {
                    if (!silent_mode) printf("Failed to create thread for file: %s\n", file_path);
                    free(targ);
                } else {
                    CloseHandle(thread);
                }
            }
        } while (FindNextFileA(hFind, &fd) != 0);

        FindClose(hFind);
    #else
        DIR *dir = opendir(dir_path);
        if (!dir) {
            if (!silent_mode) printf("Unable to open directory: %s\n", dir_path);
            return;
        }
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            const char *dot = strrchr(entry->d_name, '.');
            if (dot && strcmp(dot, ".dat") == 0) {
                char file_path[1024];
                snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, entry->d_name);

                // Allocate memory for thread arguments
                thread_arg_t *targ = malloc(sizeof(thread_arg_t));
                if (!targ) {
                    if (!silent_mode) printf("Memory allocation failed for thread arguments.\n");
                    continue;
                }
                SAFE_STRNCPY(targ->filepath, sizeof(targ->filepath), file_path);
                SAFE_STRNCPY(targ->output_dir, sizeof(targ->output_dir), output_dir);

                // Create a thread to process the file
                pthread_t thread;
                if (pthread_create(&thread, NULL, thread_process_file, targ) != 0) {
                    if (!silent_mode) printf("Failed to create thread for file: %s\n", file_path);
                    free(targ);
                } else {
                    pthread_detach(thread);
                }
            }
        }
        closedir(dir);
    #endif
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        if (!silent_mode) printf("Usage: %s [-s] <file or directory path>\n", argv[0]);
        return 1;
    }

    int arg_index = 1;

    // Check for silent mode option
    if (strcmp(argv[arg_index], "-s") == 0) {
        silent_mode = 1;
        arg_index++;
        if (argc < arg_index + 1) {
            if (!silent_mode) printf("Usage: %s [-s] <file or directory path>\n", argv[0]);
            return 1;
        }
    }

    const char *path = argv[arg_index];

    struct stat path_stat;
    if (stat(path, &path_stat) != 0) {
        if (!silent_mode) printf("Path does not exist: %s\n", path);
        return 1;
    }

    // Process directory or single file based on the input path
    if (IS_DIR(path_stat.st_mode)) {
        process_directory(path);
    }
    else if (IS_REG(path_stat.st_mode)) {
        const char *dot = strrchr(path, '.');
        if (!dot || strcmp(dot, ".dat") != 0) {
            if (!silent_mode) printf("File is not a .dat file: %s\n", path);
            return 1;
        }
        char output_dir[1024];
        const char *last_sep = strrchr(path, '/');
        #ifdef _WIN32
            if (!last_sep) {
                last_sep = strrchr(path, '\\');
            }
        #endif
        char dir_part[1024] = {0};
        if (last_sep) {
            size_t dir_len = last_sep - path + 1;
            if (dir_len < sizeof(dir_part)) {
                SAFE_STRNCPY(dir_part, sizeof(dir_part), path);
                dir_part[dir_len] = '\0';
                if (dir_len + DECODING_SUFFIX_LEN < sizeof(output_dir)) {
                    snprintf(output_dir, sizeof(output_dir), "%s%s", dir_part, DECODING_SUFFIX);
                } else {
                    if (!silent_mode) printf("Output directory path too long: %s%s\n", dir_part, DECODING_SUFFIX);
                    return 1;
                }
            } else {
                if (!silent_mode) printf("Path too long: %s\n", path);
                return 1;
            }
        } else {
            if (DECODING_SUFFIX_LEN + 1 < sizeof(output_dir)) {
                snprintf(output_dir, sizeof(output_dir), "%s", DECODING_SUFFIX);
            }
            else {
                if (!silent_mode) printf("Output directory path too long: %s\n", DECODING_SUFFIX);
                return 1;
            }
        }

        // Allocate memory for thread arguments
        thread_arg_t *targ = malloc(sizeof(thread_arg_t));
        if (!targ) {
            if (!silent_mode) printf("Memory allocation failed for thread arguments.\n");
            return 1;
        }
        SAFE_STRNCPY(targ->filepath, sizeof(targ->filepath), path);
        SAFE_STRNCPY(targ->output_dir, sizeof(targ->output_dir), output_dir);

        // Create a thread to process the single file
        #ifdef _WIN32
            HANDLE thread;
            thread = (HANDLE)_beginthreadex(NULL, 0, thread_process_file, targ, 0, NULL);
            if (thread == NULL) {
                if (!silent_mode) printf("Failed to create thread for file: %s\n", path);
                free(targ);
                return 1;
            } else {
                WaitForSingleObject(thread, INFINITE);
                CloseHandle(thread);
            }
        #else
            pthread_t thread;
            if (pthread_create(&thread, NULL, thread_process_file, targ) != 0) {
                if (!silent_mode) printf("Failed to create thread for file: %s\n", path);
                free(targ);
                return 1;
            } else {
                pthread_join(thread, NULL);
            }
        #endif
    }
    else {
        if (!silent_mode) printf("Invalid path: %s\n", path);
        return 1;
    }

    return 0;
}
