#include <stdio.h>
#include <ext2fs/ext2_fs.h>
#include <ext2fs/ext2fs.h>
#include <stdlib.h>
#include <locale.h>
#include <sys/stat.h>  // Для макросов S_ISREG, S_ISDIR, S_ISLNK и т.д.
#include <zlib.h>

//#define BLOCK_SIZE 4096 

typedef struct {
    ext2_filsys fs;
    struct ext2_dir_entry *dirent;
} PrivateData;

unsigned int BLOCK_SIZE; 

struct Marker{
    unsigned long direct;
    unsigned long char_dev_files;
    unsigned long block_dev_files;
    unsigned long fifos;
    unsigned long link;
    unsigned long symlink;
    unsigned long sockets;
    unsigned long reg_files;
     unsigned long uncnows_files;
};

void check_gr_desc_and_inodes_and_data(ext2_filsys *fs);

void check_blocks(ext2_filsys fs, ext2_ino_t dir);

int process_dir_entry(struct ext2_dir_entry *dirent, int offset, int blocksize, char *buf, void *private);

errcode_t check_data_blocks (ext2_filsys fs, ext2_ino_t inode_num, struct ext2_inode *inode);

int process_block(ext2_filsys fs, blk_t *blocknr, int blockcnt, void *private);

unsigned long calculate_block_checksum(const char *filename, off_t block_offset, size_t block_size);

void print_filesystem_info(ext2_filsys *fs); //вывод информации про ФС

void count_file_types(struct Marker *cnt_types, ext2_filsys *fs);

int main(int argc, char **argv){
    setlocale(LC_ALL, "Rus");
    ext2_filsys fs; //дескриптор файловой системы | объявляешь переменную ext2_filsys fs - создаешь указатель на структуру struct struct_ext2_filsys
    
    errcode_t err;
    int mount_flags;
    int total_numbers_inodes;
    if (argc != 2) {
        fprintf(stderr, "%s <device>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    errcode_t err = ext2fs_check_if_mounted(argv[1], &mount_flags); //Проверяем смонтировано ли устройство
    if (err) {
        fprintf(stderr, "Ошибка при проверке монтирования устройства: %s\n", error_message(err));
        return 1;
    }

    if (mount_flags) {
        printf("Устройство смонтировано. Размонтируйте его\n");
        return 1;
    } 

    err = ext2fs_open(argv[1], EXT2_FLAG_RW, 0, 0, NULL, &fs); //Открытие ФС
    if (err) {
        fprintf(stderr, "Ошибка открытия файловой системы: %s\n", error_message(err));
        exit(EXIT_FAILURE);
    }

    if(fs->super->s_magic != EXT2_SUPER_MAGIC){
        printf("Неизвестный тип файловой системы\n");
    }

    BLOCK_SIZE = fs->blocksize;

    check_gr_desc_and_inodes_and_data(&fs);




    print_filesystem_info(&fs); 

    err = ext2fs_close(fs);//Закрытие ФС
    if(err){
        fprintf(stderr, "Ошибка закрытия файловой системы: %s\n", error_message(err));
        exit(EXIT_FAILURE);
    }
}

void print_filesystem_info(ext2_filsys *fs){
    prinf("%s: **** ИНФОРМАЦИЯ ОБ УСТРОЙСТВЕ ****\n\n", (*fs)->device_name);
    printf("использовано inodes: %d out of %d\n", (*fs)->super->s_inodes_count - (*fs)->super->s_free_inodes_count, (*fs)->super->s_blocks_count); //20 inodes used out of 192000)
    printf("использовано блоков: %d out of %d", (*fs)->super->s_blocks_count - (*fs)->super->s_free_blocks_count, (*fs)->super->s_blocks_count); // 30884 blocks used (4.02%, out of 768000)
    printf("размер блока: %d",(*fs)->blocksize);
    //printf("bad блоков: %d", (*fs)->badblocks->count);

    struct Marker cnt_filetype = { 0 };
    count_file_types(&cnt_filetype, fs);

    printf("Обычные файлы: %d\n", cnt_filetype.reg_files);
    printf("Каталоги: %d\n", cnt_filetype.direct);
    printf("Символьные устройства: %d\n", cnt_filetype.char_dev_files);
    printf("Блочные устройства: %d\n", cnt_filetype.block_dev_files);
    printf("FIFOs: %d\n", cnt_filetype.fifos);
    printf("Сокеты: %d\n", cnt_filetype.sockets);
    printf("Символические ссылки: %d\n", cnt_filetype.symlink);
    printf("---------------\nВсего файлов: %d", cnt_filetype.block_dev_files + cnt_filetype.char_dev_files + cnt_filetype.direct + cnt_filetype.fifos + 
    cnt_filetype.symlink + cnt_filetype.reg_files + cnt_filetype.sockets + cnt_filetype.uncnows_files);
}

void count_file_types(struct Marker *cnt_types, ext2_filsys *fs){

    struct ext2_inode inode;

    for(ext2_ino_t i = EXT2_FIRST_INO((*fs)->super); i <= (*fs)->super->s_inodes_count; i++){
        if(ext2fs_test_inode_bitmap((*fs)->inode_map, i) == 1){ //inode действующий
            ext2fs_read_inode((*fs), i, &inode);       //тут моежт быть проверка на целостность. Может и не считаться
             // Определение типа файла
            if (S_ISREG(inode.i_mode))//
                (*cnt_types).reg_files++;
            else if (S_ISDIR(inode.i_mode)) //
                (*cnt_types).direct++;
            else if (S_ISCHR(inode.i_mode))//
               (*cnt_types).char_dev_files++;
            else if (S_ISBLK(inode.i_mode))//
                (*cnt_types).block_dev_files++;
            else if (S_ISFIFO(inode.i_mode))//
                (*cnt_types).fifos++;
            else if (S_ISLNK(inode.i_mode))//
                (*cnt_types).symlink++;
            else if (S_ISSOCK(inode.i_mode))
                (*cnt_types).sockets++;
            else
               (*cnt_types).uncnows_files++;
        }
    }
}

void check_gr_desc_and_inodes_and_data(ext2_filsys *fs){
    errcode_t err;

    err = ext2fs_check_desc(*fs); //Эта функция проверяет структуру дескрипторов файловой системы на целостность.
    if (err) {
        fprintf(stderr, "Ошибка при проверке целостности дескрипторов файловой системы: %s\n", error_message(err));
         exit(EXIT_FAILURE);
        // Дальнейшие действия для восстановления или обработки ошибки
        // Например, попытка восстановления из резервных копий
    }

    check_blocks(*fs, EXT2_ROOT_INO);

}

void check_blocks(ext2_filsys fs, ext2_ino_t dir) {
    errcode_t err;
    struct ext2_inode inode;
    int flags = 0;
    PrivateData private_data = {fs, NULL};
    while (ext2fs_dir_iterate(fs, dir, flags, NULL, process_dir_entry, &private_data) == 0) {
        if (private_data.dirent->inode == 0)
            continue;

        if (private_data.dirent->name[0] == '.')
            continue;

        ext2_ino_t inode_num = private_data.dirent->inode;

        err = ext2fs_read_inode(fs, inode_num, &inode);
        if (err) {
            fprintf(stderr, "Ошибка чтения inode %u: %s\n", inode_num, error_message(err));
            continue;
        }

        if (S_ISDIR(inode.i_mode))
        {
            check_blocks(fs, inode_num);
        }

        // Проверяем блоки данных inode
        if(S_ISDIR(inode.i_mode) || S_ISREG(inode.i_mode))
        {
            err = check_data_blocks(fs, inode_num, &inode);
            if (err) {
                fprintf(stderr, "Ошибка проверки блоков данных inode %u - файла %s: %s\n", inode_num, private_data.dirent->name, error_message(err));
                continue;
            }
        }
    }
}

int process_dir_entry(struct ext2_dir_entry *dirent, int offset, int blocksize, char *buf, void *private) {
   
    struct ext2_inode inode;
    errcode_t err;
    PrivateData *private_data = (PrivateData *)private;
    ext2_filsys fs = private_data->fs;
    private_data->dirent = dirent;

    err = ext2fs_read_inode(fs, dirent->inode, &inode);
    //printf("Directory entry name: %.*s\n", dirent->name_len, dirent->name);
    if (!S_ISCHR(inode.i_mode) && !S_ISBLK(inode.i_mode) && !S_ISLNK(inode.i_mode)) //S_ISCHR   S_ISBLK  S_ISLNK
    {   
         // Проверяем, является ли inode действующим в inode bitmap
        if (!ext2fs_test_inode_bitmap(fs->inode_map, dirent->inode)) {
            printf("Файловый inode не является действительным: %u\n", dirent->inode);
        }
         
        if(ext2_inode_has_valid_blocks (&inode) == 0){
            printf("Блоки данных файла не действительны\nфайл: %.*s\n", dirent->name_len, dirent->name);
        }
    }
    return 0;
}


errcode_t check_data_blocks(ext2_filsys fs, ext2_ino_t inode_num, struct ext2_inode *inode){
    
    errcode_t err;
    int found_bad_block = 0;
     unsigned long block_offset = 0;
    unsigned long block_checksum;
    // Итерируемся по блокам данных inode
    err = ext2fs_block_iterate(fs, inode_num, BLOCK_FLAG_DATA_ONLY, NULL, process_block, &found_bad_block);
    if (err)
        return err;

    for (blk_t blocknr = 0; blocknr < inode->i_blocks; blocknr++) {
        block_checksum = calculate_block_checksum(fs->device_name, block_offset, BLOCK_SIZE);
        //printf("Контрольная сумма блока данных inode %d, блок %u: %lx\n", inode_num, blocknr, block_checksum);
        if(block_checksum ! = ){ //???
            found_bad_block
        }
        block_offset += BLOCK_SIZE;
    }

    printf("Обнаруженыe битые блоки данных inode %d\n", inode_num);

    return 0;

}

int process_block(ext2_filsys fs, blk_t *blocknr, int blockcnt, void *private) {
    int *found_bad_block = (int *)private;
    errcode_t err;

    // Проверяем, является ли блок битым
    if (ext2fs_test_block_bitmap(fs->block_map, *blocknr) == 0) {
        //printf("Битый блок: %u\n", *blocknr);
        *found_bad_block++;
    }

    return 0;
}

unsigned long calculate_block_checksum(const char *filename, off_t block_offset, size_t block_size) { //block_offset: Смещение блока данных от начала файла.
    FILE *input_file;
    unsigned long crc = crc32(0L, Z_NULL, 0); // Инициализация переменной crc значением 0.
    unsigned char *buffer;               //- Объявление указателя на буфер для чтения данных из файла.
    size_t bytes_read;                   //Объявление переменной для хранения количества байт, прочитанных из файла.

    input_file = fopen(filename, "r");
    if (input_file == NULL) {
        fprintf(stderr, "Error opening file %s: %s\n", filename, strerror(errno));
        return crc;
    }

    if (fseeko(input_file, block_offset, SEEK_SET) == -1) {      //fseeko() используется вместо fseek() для работы с большими файлами
        fprintf(stderr, "Error seeking to block offset: %s\n", strerror(errno));
        fclose(input_file);
        return crc;
    }

    buffer = (unsigned char *)malloc(block_size);
    if (buffer == NULL) {
        fprintf(stderr, "Error allocating memory for buffer\n");
        fclose(input_file);
        return crc;
    }

    while ((bytes_read = fread(buffer, 1, block_size, input_file)) > 0) {
        crc = crc32(crc, buffer, (uInt)bytes_read);
        if (bytes_read < block_size) {
            if (feof(input_file)) {
                // Reached end of file
                break;
            } else if (ferror(input_file)) {
                // Read error
                fprintf(stderr, "Error reading file: %s\n", strerror(errno));
                break;
            }
        }
    }

    free(buffer);
    fclose(input_file);

    return crc;
}
