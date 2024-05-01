#include <stdio.h>
#include <unistd.h>  // Для функций lseek, close
#include <ext2fs/ext2_fs.h>
#include <ext2fs/ext2fs.h>
#include <stdlib.h>
#include <locale.h>
#include <sys/stat.h>  // Для макросов S_ISREG, S_ISDIR, S_ISLNK и т.д.
//#include <zlib.h>
#include <fcntl.h>
#include <sys/types.h>

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


int read_block(ext2_filsys *fs, blk_t block_num, char* buf); //вернёт 0 если всё ок

void check_csum_fs(ext2_filsys *fs); //ПРОЕВРКА КОНТРОЛЬНОЙ СУММЫ ГРУППОВЫХ ДЕСКРИПТОРОВ И СУПЕРБЛОКА(ОСНОВНОГО) И bitmap inodes and data block

void check_gr_desc_and_inodes_and_data(ext2_filsys *fs);//ПРОВЕРКА СТРУКТУРЫ ДЕСКРИПТОРОВ ГРУПП 

void check_blocks(ext2_filsys fs, ext2_ino_t dir);// ПРОВЕРКА ТОГО, ЧТО СУЩЕСВТУЮЩИЕ ФАЙЛЫ ПОМЕЧЕНЫ КАК ИСПОЛЬЗУЕМЫЕ В inode- и block bitmap 

int process_dir_entry(struct ext2_dir_entry *dirent, int offset, int blocksize, char *buf, void *private); //ФУНКЦИЯ КОТОРАЯ ВЫЗЫВАЕТСЯ КОГДА ИТЕРИРУЕМСЯ ПО КАТАЛОГУ(ПРОВЕРКА ЧТО Блоки данных файла действительны и Файловый inode не является действительным)

errcode_t check_data_blocks(ext2_filsys fs, ext2_ino_t inode_num, struct ext2_inode *inode, int *found_bad_block); //ТУТ МЫ Итерируемся по блокам данных inode 

int process_block(ext2_filsys fs, blk_t *blocknr, int blockcnt, void *private); // сверяемс блоки данных файла с block_bitmap


//................ПОКА ТОЛЬКО ИДЕЯ.........................//

//void check_sum_entry_dir();

void repair_bitmaps(ext2_filsys* fs); // НЕ РЕАЛИЗОВАННАЯ ИДЕЯ. МОЖНО ПОЧИНИТЬ ПРОХОДЯСЬ ПО ВСЕМ inodes и их i_mode и i_dtame и сверяясь по функции ext2fs_test_inode_bitmap


//................УЖЕ ПРОТЕСТИРОВАНО И РАБОТАЕТ..................//
void print_filesystem_info(ext2_filsys *fs); //вывод информации про ФС

void count_file_types(struct Marker *cnt_types, ext2_filsys fs);

void open_fs(ext2_filsys* fs, char*);

void read_bitmaps(ext2_filsys* fs);

void write_bitmaps(ext2_filsys* fs);



int main(int argc, char **argv){
    setlocale(LC_ALL, "Rus");
    ext2_filsys fs = NULL; //дескриптор файловой системы | объявляешь переменную ext2_filsys fs - создаешь указатель на структуру struct struct_ext2_filsys
    printf("Начало\n");
    
    

    if (argc != 2) {
        fprintf(stderr, "%s <device>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

     
    open_fs(&fs, argv[1]);

    printf("ШАГ1: Проверка целостноcти блоков данных, групповых дескрипторов\n");
    check_gr_desc_and_inodes_and_data(&fs);
    
    printf("ШАГ 2: Проверка контрольных сумм суперблока, групповых дескрипторов, bitmap, inode table\n");
    check_csum_fs(&fs);

    //printf("ШАГ 3: Проверка структуры каталогов\n");

    print_filesystem_info(&fs); 
    errcode_t err = ext2fs_close(fs);//Закрытие ФС
    if(err){
        fprintf(stderr, "Ошибка закрытия файловой системы: %s\n", error_message(err));
        exit(EXIT_FAILURE);
    }
    
    return 0;
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
    int found_bad_block = 0;
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
            err = check_data_blocks(fs, inode_num, &inode, &found_bad_block);
            if (err) {
                fprintf(stderr, "Ошибка проверки блоков данных inode %u - файла %s: %s\n", inode_num, private_data.dirent->name, error_message(err));
                continue;
            }
        }
    }
    printf("Обнаруженыe битые блоки %d\n", found_bad_block);
}

int process_dir_entry(struct ext2_dir_entry *dirent, int offset, int blocksize, char *buf, void *private) {
   
    struct ext2_inode inode;
    errcode_t err;
    PrivateData *private_data = (PrivateData *)private;
    ext2_filsys fs = private_data->fs;
    private_data->dirent = dirent;
    
    err = ext2fs_read_inode(fs, dirent->inode, &inode);
    if (!S_ISCHR(inode.i_mode) && !S_ISBLK(inode.i_mode) && !S_ISLNK(inode.i_mode)) //S_ISCHR   S_ISBLK  S_ISLNK
    {   
         // Проверяем, является ли inode действующим в inode bitmap
        if (!ext2fs_test_inode_bitmap(fs->inode_map, dirent->inode)) {
            printf("Файловый inode не является действительным: %u\nфайл: %s", dirent->inode, dirent->name);
        }
        
        if(ext2fs_inode_has_valid_blocks(&inode) == 0){
            printf("Блоки данных файла не действительны\nфайл: %.*s\n", dirent->name_len, dirent->name);
        }
    }
    return 0;
}


errcode_t check_data_blocks(ext2_filsys fs, ext2_ino_t inode_num, struct ext2_inode *inode, int *found_bad_block){
    
    errcode_t err;
    //int found_bad_block = 0;
     unsigned long block_offset = 0;
    unsigned long block_checksum;
    // Итерируемся по блокам данных inode
    err = ext2fs_block_iterate(fs, inode_num, BLOCK_FLAG_DATA_ONLY, NULL, process_block, found_bad_block);
    if (err)
        return err;

    return 0;

}

int process_block(ext2_filsys fs, blk_t *blocknr, int blockcnt, void *private) {
    int *found_bad_block = (int *)private;
    errcode_t err;

    // Проверяем, отмечен ли используемы блок в карте блоков   
    if (ext2fs_test_block_bitmap(fs->block_map, *blocknr) == 0) {
        //printf("Битый блок: %u\n", *blocknr);
        (*found_bad_block)++;
    }
    return 0;
}


////////////////////////////////////

void check_csum_fs(ext2_filsys *fs){
    errcode_t err;
    err = ext2fs_verify_csum_type(*fs, (*fs)->super);
    if(err){
        printf("Тип контрольной суммы не поддерживается файловой системой\n");
        return;
    }

    err = ext2fs_superblock_csum_verify((*fs), (*fs)->super); // Проверяет контрольную сумму суперблока.
    if(err){
        printf("Контрольная сумма суперблока не совпала с вычисленной контрольной суммой\n");
        //заменяем на резервную копию из другой группы блоков 
    }

    for(dgrp_t num_group = 0; num_group < (*fs)->group_desc_count; num_group++){
        err = ext2fs_group_desc_csum_verify((*fs), num_group);
        if(err){
            printf("Контрольная сумма группы блоков №%d не совпала с вычисленной\n", num_group); 
            //заменяем на копию из другой группы блоков
        }

        struct ext2_group_desc *group_desc = ext2fs_group_desc((*fs), (*fs)->group_desc, num_group); //возвращает указатель на структуру ext2_group_desc, которая содержит информацию о группе блоков group.
        __u32 block_bitmap_block = (*group_desc).bg_block_bitmap;
        __u32 inode_bitmap_block = (*group_desc).bg_inode_bitmap;

        char* bitmap_buffer = NULL;
        bitmap_buffer = malloc(BLOCK_SIZE);
        
        if(read_block(fs, block_bitmap_block, bitmap_buffer)){
            printf("Ошибка считывания растрового изображения карты блоков группы блоков №%d\n", num_group);
            continue;
        }

        err = ext2fs_block_bitmap_csum_verify((*fs), num_group, bitmap_buffer, BLOCK_SIZE);
        if(err){
            printf("Контрольная сумма карты блоков группы блоков №%d не совпало с вычисленной\n", num_group);
            //continue;
            //как-то обработать - возможно добавить в bad blocks либо просто пометить
        }

        if(read_block(fs, inode_bitmap_block, bitmap_buffer)){
            printf("Ошибка считывания растрового изображения карты блоков группы блоков №%d\n", num_group);
            continue;
        }

        err = ext2fs_inode_bitmap_csum_verify((*fs), num_group, bitmap_buffer, BLOCK_SIZE);
        if(err){
            printf("Контрольная сумма карты inodes группы блоков №%d не совпало с вычисленной\n", num_group);
            //continue;
            //как-то обработать - возможно добавить в bad blocks либо просто пометить
        }

        free(bitmap_buffer);
    }

}

int read_block(ext2_filsys *fs, __u32 block_num, char* buf){
    int fd;
    ssize_t bytes_read;

    fd = open((*fs)->device_name, O_RDONLY);
    if (fd < 0) {
        printf("Failed to open device\n");
        return -1;
    }

    if (block_num < 0 || block_num > (*fs)->super->s_blocks_count) {
        printf("Invalid block number\n");
        return -1;
    }

    off_t offset = block_num * BLOCK_SIZE;

    if (lseek(fd, offset, SEEK_SET) == -1) {
        printf("Failed to seek to block\n");
        close(fd);
        return -1;
    }

    bytes_read = read(fd, buf, BLOCK_SIZE);
    if (bytes_read != BLOCK_SIZE) {
        printf("Failed to read block\n");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}



void repair_bitmaps(ext2_filsys* fs){

}

//.............................УЖЕ ПРОТЕСТИРОВАНО И РАБОТАЕТ.................................//
void read_bitmaps(ext2_filsys* fs){
    errcode_t err;
    //чтение растрового изображение карты inodes
    err = ext2fs_read_inode_bitmap((*fs));
    if(err){
        fprintf(stderr, "Ошибка чтения inode_map: %s\n", error_message(err));
        exit(EXIT_FAILURE);
    }

    //чтение растрового изображения карты блоков
    err = ext2fs_read_block_bitmap(*fs);
    if(err){
        fprintf(stderr, "Ошибка чтения block_bitmap: %s\n", error_message(err));
        exit(EXIT_FAILURE);
    }

}

void write_bitmaps(ext2_filsys* fs){
    errcode_t err;
    //запись растрового изображения карты inodes
    err = ext2fs_write_inode_bitmap(*fs);
    if(err){
        fprintf(stderr, "Ошибка записи inode_map на диск: %s\n", error_message(err));
        exit(EXIT_FAILURE);
    }

    //запись астрового изображения карты блоков на диск
    err = ext2fs_write_block_bitmap(*fs);
    if(err){
        fprintf(stderr, "Ошибка записи block_bitmap на диск: %s\n", error_message(err));
        exit(EXIT_FAILURE);
    }

}


void open_fs(ext2_filsys* fs, char* name_device){

    errcode_t err = 0;
    int mount_flags = 0;
    err = ext2fs_check_if_mounted(name_device, &mount_flags); //Проверяем смонтировано ли устройство
    if (err) {
        fprintf(stderr, "Ошибка при проверке монтирования устройства: %s\n", error_message(err));
        exit(EXIT_FAILURE);
    }

    if (mount_flags) {
        printf("Устройство смонтировано. Размонтируйте его\n");
        exit(EXIT_FAILURE);
    } 
   

    err = ext2fs_open(name_device, EXT2_FLAG_RW, 0, 0, unix_io_manager, fs); //Открытие ФС
    if (err) {
        fprintf(stderr, "Ошибка открытия файловой системы: %s\n", error_message(err));
        exit(EXIT_FAILURE);
    }
    
    read_bitmaps(fs); //считывание с диска растровых изображений
    
    if((*fs)->super->s_magic != EXT2_SUPER_MAGIC){
        printf("Неизвестный тип файловой системы\n");
        exit(EXIT_FAILURE);
    }
    BLOCK_SIZE = (*fs)->blocksize;
}


void print_filesystem_info(ext2_filsys *fs) {
    printf("%s: **** ИНФОРМАЦИЯ ОБ УСТРОЙСТВЕ ****\n\n", (*fs)->device_name);
    printf("использовано inodes: %d out of %d\n", (*fs)->super->s_inodes_count - (*fs)->super->s_free_inodes_count, (*fs)->super->s_inodes_count); //20 inodes used out of 192000)
    printf("использовано блоков: %d out of %d\n", (*fs)->super->s_blocks_count - (*fs)->super->s_free_blocks_count, (*fs)->super->s_blocks_count); // 30884 blocks used (4.02%, out of 768000)
    printf("размер блока: %d", (*fs)->blocksize);
    // printf("bad блоков: %d", (*fs)->badblocks->count);

    struct Marker cnt_filetype = { 0 };
    count_file_types(&cnt_filetype, *fs);

    printf("Обычные файлы: %d\n", cnt_filetype.reg_files);
    printf("Каталоги: %d\n", cnt_filetype.direct);
    printf("Символьные устройства: %d\n", cnt_filetype.char_dev_files);
    printf("Блочные устройства: %d\n", cnt_filetype.block_dev_files);
    printf("FIFOs: %d\n", cnt_filetype.fifos);
    printf("Сокеты: %d\n", cnt_filetype.sockets);
    printf("Символические ссылки: %d\n", cnt_filetype.symlink);
    printf("Неизвестные файла: %d\n", cnt_filetype.uncnows_files);
    printf("---------------\nВсего файлов: %d", cnt_filetype.block_dev_files + cnt_filetype.char_dev_files + cnt_filetype.direct + cnt_filetype.fifos + cnt_filetype.symlink + cnt_filetype.reg_files + cnt_filetype.sockets);
}


void count_file_types(struct Marker *cnt_types, ext2_filsys fs) {
    // Убедитесь, что fs не является нулевым указателем
    if (fs == NULL) {
        fprintf(stderr, "Ошибка: файловая система не инициализирована\n");
        exit(EXIT_FAILURE);
    }

    // Проверьте, что inode_map инициализирован
    if (fs->inode_map == NULL) {
        fprintf(stderr, "Ошибка: inode_map не инициализирован\n");
        exit(EXIT_FAILURE);
    }
    //ext2fs_read_inode_bitmap(fs);
  
    errcode_t err;
    ext2_ino_t i;

   
    for (i = EXT2_FIRST_INO(fs->super); i <= fs->super->s_inodes_count; i++) {
        if (!ext2fs_test_inode_bitmap(fs->inode_map, i))  //inode действующий
            continue;

            struct ext2_inode inode;
            err = ext2fs_read_inode(fs, i, &inode);       //тут моежт быть проверка на целостность. Может и не считаться
            if (err) {
                fprintf(stderr, "Ошибка чтения inode %u: %s\n", i, error_message(err));
                continue;
            }
            // Определение типа файла
            if (S_ISREG(inode.i_mode))//
                    cnt_types->reg_files++;
            else if (S_ISDIR(inode.i_mode)) //
                cnt_types->direct++;
            else if (S_ISCHR(inode.i_mode))//
                cnt_types->char_dev_files++;
            else if (S_ISBLK(inode.i_mode))//
                cnt_types->block_dev_files++;
            else if (S_ISFIFO(inode.i_mode))//
                cnt_types->fifos++;
            else if (S_ISLNK(inode.i_mode))//
                cnt_types->symlink++;
            else if (S_ISSOCK(inode.i_mode))
                cnt_types->sockets++;
            else
                cnt_types->uncnows_files++;
    }
   
}
