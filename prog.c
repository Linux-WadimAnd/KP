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
#include <stdbool.h>

typedef struct {
    ext2_filsys* fs;
    int* b_blocks;
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




//................ПОКА ТОЛЬКО ИДЕЯ.........................//

bool flag_need_repair_bitmaps = false; //если обнаружится что файл по факту действующий, но в inode_bitmap он не отмечен, значит флаг стнет true и надо это исправить в функции void repair_bitmaps(ext2_filsys* fs)

//void check_sum_entry_dir();

void repair_bitmaps(ext2_filsys* fs); // НЕ РЕАЛИЗОВАННАЯ ИДЕЯ. МОЖНО ПОЧИНИТЬ ПРОХОДЯСЬ ПО ВСЕМ inodes и их i_mode и i_dtame и сверяясь по функции ext2fs_test_inode_bitmap

//................УЖЕ ПРОТЕСТИРОВАНО И РАБОТАЕТ..................//

void check_csum_fs(ext2_filsys *fs); //ПРОЕВРКА КОНТРОЛЬНОЙ СУММЫ ГРУППОВЫХ ДЕСКРИПТОРОВ И СУПЕРБЛОКА(ОСНОВНОГО) И bitmap inodes and data block

void print_filesystem_info(ext2_filsys *fs); //вывод информации про ФС

void count_file_types(struct Marker *cnt_types, ext2_filsys fs);

void open_fs(ext2_filsys* fs, char*);

void read_bitmaps(ext2_filsys* fs);

void write_bitmaps(ext2_filsys* fs);

void check_gr_desc_and_inodes_and_data(ext2_filsys *fs);//ПРОВЕРКА СТРУКТУРЫ ДЕСКРИПТОРОВ ГРУПП 

void check_all_files(ext2_filsys* fs, ext2_ino_t dir, int* b_blocks);// ПРОВЕРКА ТОГО, ЧТО СУЩЕСВТУЮЩИЕ ФАЙЛЫ ПОМЕЧЕНЫ КАК ИСПОЛЬЗУЕМЫЕ В inode- и block bitmap 

int process_dir_entry(struct ext2_dir_entry *dirent, int offset, int blocksize, char *buf, void *private); //ФУНКЦИЯ КОТОРАЯ ВЫЗЫВАЕТСЯ КОГДА ИТЕРИРУЕМСЯ ПО КАТАЛОГУ(ПРОВЕРКА ЧТО Блоки данных файла действительны и Файловый inode не является действительным)

int test_block(ext2_filsys fs, blk_t *blocknr, int blockcnt, void *private); // сверяемс блоки данных файла с block_bitmap

void close_fs(ext2_filsys* fs);

static __u32 ext2fs_superblock_csum(ext2_filsys fs, struct ext2_super_block *sb);

int ext2fs_superblock_csum_my_verify(ext2_filsys fs, struct ext2_super_block* sb); //оригинал из библиотеки ext2fs всегда выдаёт неправильно

int ext2fs_superblock_csum_my_verify(ext2_filsys fs, struct ext2_super_block* sb); //1 в случае совпадения, 0 в случае несовпадения


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

    printf("ШАГ 2: Проверка контрольных сумм суперблока, групповых дескрипторов, inode_bitmap, block_bitmap\n");
    check_csum_fs(&fs);

    //printf("ШАГ 3: Проверка структуры каталогов\n");

    print_filesystem_info(&fs); 

    close_fs(&fs);

   
    return 0;
}


//////////////////////////////////////




void repair_bitmaps(ext2_filsys* fs){

}

//.............................УЖЕ ПРОТЕСТИРОВАНО И РАБОТАЕТ.................................//


static __u32 ext2fs_superblock_csum(ext2_filsys fs, struct ext2_super_block *sb)
{
	int offset = offsetof(struct ext2_super_block, s_checksum);

	return ext2fs_crc32c_le(~0, (unsigned char *)sb, offset);
}

void check_csum_fs(ext2_filsys *fs){
    
    errcode_t err = 0;
    read_bitmaps(fs);

    if(!ext2fs_verify_csum_type(*fs, (*fs)->super)){
        printf("Тип контрольной суммы не поддерживается файловой системой\n");
        return;
    }

    if(!ext2fs_superblock_csum_verify(*fs, (*fs)->super)){
        printf("Контрольная сумма суперблока не совпала с вычисленной контрольной суммой\n");
        return;
    }

    if(!ext2fs_superblock_csum_my_verify(*fs, (*fs)->super)){
        printf("Контрольная сумма суперблока не совпала с вычисленной контрольной суммой\n");
        return;
    }

    if ((*fs)->inode_map == NULL) {
        printf("Ошибка: inode_map не инициализирован\n");
        return;
    }
    if((*fs)->block_map == NULL){
        printf("Ошибка: inode_map не инициализирован\n");
        return;
    }

    for(dgrp_t num_group = 0; num_group < (*fs)->group_desc_count; num_group++){

        if(!ext2fs_group_desc_csum_verify((*fs), num_group)){
            printf("Контрольная сумма группы блоков №%d не совпала с вычисленной\n", num_group); 
            //заменяем на копию из другой группы блоков
            continue;
        }
        
        struct ext4_group_desc *gdp = (struct ext4_group_desc *)ext2fs_group_desc(*fs, (*fs)->group_desc, num_group);

        __u32 provided_checksum_inode = gdp->bg_inode_bitmap_csum_lo;

        __u32 inode_bitmap_checksum = ext2fs_inode_bitmap_checksum(*fs, num_group);

         if ((*fs)->super->s_desc_size >= EXT4_BG_INODE_BITMAP_CSUM_HI_END)
            provided_checksum_inode |= (__u32)gdp->bg_inode_bitmap_csum_hi << 16;
             // Сравниваем контрольные суммы
        if (inode_bitmap_checksum != provided_checksum_inode) {
            printf("Контрольная сумма карты инодов группы блоков №%d не совпала с вычисленной\n", num_group);
            printf("Вычислено = %lx, указана в структуре данных = %ls\n", inode_bitmap_checksum, provided_checksum_inode);
        }

        __u32 block_bitmap_checksum = ext2fs_block_bitmap_checksum(*fs, num_group);
                                        
        __u32 provided_checsum_block = gdp->bg_block_bitmap_csum_lo;

        if ((*fs)->super->s_desc_size >= EXT4_BG_BLOCK_BITMAP_CSUM_HI_LOCATION)
                 provided_checsum_block |= (__u32)gdp->bg_block_bitmap_csum_hi << 16;
            
        if (block_bitmap_checksum != provided_checsum_block) {
            printf("Контрольная сумма карты блоков группы блоков №%d не совпала с вычисленной\n", num_group);
            printf("Вычислено = %lx, указана в структуре данных = %ls\n", block_bitmap_checksum, provided_checsum_block);
        }
    }

}

void read_bitmaps(ext2_filsys* fs){
    errcode_t err;
    //чтение растрового изображение карты inodes
    err = ext2fs_read_inode_bitmap((*fs));
    if(err){
        fprintf(stderr, "Ошибка чтения inode_map: %s\n", error_message(err));
        close_fs(fs);
        exit(EXIT_FAILURE);
    }

    //чтение растрового изображения карты блоков
    err = ext2fs_read_block_bitmap(*fs);
    if(err){
        fprintf(stderr, "Ошибка чтения block_bitmap: %s\n", error_message(err));
        close_fs(fs);
        exit(EXIT_FAILURE);
    }

}

void write_bitmaps(ext2_filsys* fs){
    errcode_t err;
    //запись растрового изображения карты inodes
    err = ext2fs_write_inode_bitmap(*fs);
    if(err){
        fprintf(stderr, "Ошибка записи inode_map на диск: %s\n", error_message(err));
        close_fs(fs);
        exit(EXIT_FAILURE);
    }

    //запись астрового изображения карты блоков на диск
    err = ext2fs_write_block_bitmap(*fs);
    if(err){
        fprintf(stderr, "Ошибка записи block_bitmap на диск: %s\n", error_message(err));
        close_fs(fs);
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
        close_fs(fs);
        exit(EXIT_FAILURE);
    }

    BLOCK_SIZE = (*fs)->blocksize;
}


void print_filesystem_info(ext2_filsys *fs) {
    printf("%s: **** ИНФОРМАЦИЯ ОБ УСТРОЙСТВЕ ****\n\n", (*fs)->device_name);
    printf("использовано inodes: %d out of %d\n", (*fs)->super->s_inodes_count - (*fs)->super->s_free_inodes_count, (*fs)->super->s_inodes_count); //20 inodes used out of 192000)
    printf("использовано блоков: %d out of %d\n", (*fs)->super->s_blocks_count - (*fs)->super->s_free_blocks_count, (*fs)->super->s_blocks_count); // 30884 blocks used (4.02%, out of 768000)
    printf("размер блока: %d\n", (*fs)->blocksize);
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
    printf("---------------\nВсего файлов: %d\n", cnt_filetype.block_dev_files + cnt_filetype.char_dev_files + cnt_filetype.direct + cnt_filetype.fifos + cnt_filetype.symlink + cnt_filetype.reg_files + cnt_filetype.sockets);
}


void count_file_types(struct Marker *cnt_types, ext2_filsys fs) {
  
    if (fs == NULL) {
        fprintf(stderr, "Ошибка: файловая система не инициализирована\n");
        exit(EXIT_FAILURE);
    }

    // Проверьте, что inode_map инициализирован
    if (fs->inode_map == NULL) {
        fprintf(stderr, "Ошибка: inode_map не инициализирован\n");
        //exit(EXIT_FAILURE);
        return;
    }
  
    errcode_t err;
    ext2_ino_t i;

   
    for (i = EXT2_FIRST_INO(fs->super); i <= fs->super->s_inodes_count; i++) {

        if (!ext2fs_test_inode_bitmap(fs->inode_map, i))  //inode не действующий
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

void check_gr_desc_and_inodes_and_data(ext2_filsys *fs){

    errcode_t err;
    int b_blocks = 0;

    err = ext2fs_check_desc(*fs); //Эта функция проверяет структуру дескрипторов файловой системы на целостность.
    if (err) {
        fprintf(stderr, "Ошибка при проверке целостности дескрипторов файловой системы: %s\n", error_message(err));
         exit(EXIT_FAILURE);
        // Дальнейшие действия для восстановления или обработки ошибки
        // Например, попытка восстановления из резервных копий
    }

    check_all_files(fs, EXT2_ROOT_INO, &b_blocks); //EXT2_ROOT_INO - это макрос, представляющий номер inode корневого каталога в файловой системе Ext2
    
    printf("Блоки данных файлов(bad blocks), помеченные как неиспользуемые в bitmap: %d\n", b_blocks);

}

void check_all_files(ext2_filsys* fs, ext2_ino_t dir, int* b_blocks) {
    errcode_t err;
    int flags = 0;
    PrivateData private_data = {fs, b_blocks};
    //int found_bad_block = *b_blocks;
    err = ext2fs_dir_iterate(*fs, dir, flags, NULL, process_dir_entry, &private_data);
    if(err){
        printf("Ошибка итерации по каталогу inode = %d\n", dir);
        return;
    }
}

int process_dir_entry(struct ext2_dir_entry *dirent, int offset, int blocksize, char *buf, void *private) {

    if(dirent->name[0] == '.')
        return 0;
    errcode_t err;
    struct ext2_inode inode;
    bool flag_error = false;
    PrivateData *private_data = (PrivateData *)private;
    ext2_filsys* fs = (*private_data).fs;
    //int found_bad_block = 0;

    if (!ext2fs_test_inode_bitmap((*fs)->inode_map, dirent->inode)) {
        printf("Файловый inode = %d не отмечен в inode_bitmap как использующийся для файла %.*s", dirent->inode, dirent->name_len, dirent->name);
        flag_error = true;
    }

    err = ext2fs_read_inode(*fs, dirent->inode, &inode);
    if(err){
        printf("Ошибка считывания inode = %d файла %s\n", dirent->inode, dirent->name); //можно удалить этот файл и пометить в inode_bitmap
        return 0;
    }

    if (!S_ISCHR(inode.i_mode) && !S_ISBLK(inode.i_mode) && !S_ISLNK(inode.i_mode)) //S_ISCHR   S_ISBLK  S_ISLNK
    {   
        if(!ext2fs_inode_has_valid_blocks(&inode)){
            printf("Блоки данных файла %.*s не действительны\n", dirent->name_len, dirent->name);
            flag_error = true;
        }
    }

    // Проверяем блоки данных inode
    if(S_ISDIR(inode.i_mode) || S_ISREG(inode.i_mode))
    {   
        //err = check_data_blocks(fs, dirent->inode, &inode, private_data->b_blocks);
        err = ext2fs_block_iterate(*fs, dirent->inode, BLOCK_FLAG_DATA_ONLY, NULL, test_block, private_data->b_blocks);
        if (err) {
            if(S_ISREG(inode.i_mode))
            {
                printf("Ошибка проверки (не удалось проитерироваться) блоков данных inode %d - файла %s: %s\n", dirent->inode, dirent->name);
                return 0; 
            }
            if(S_ISDIR(inode.i_mode)){
                printf("Нарушение структуры каталогов, не удалось проитерироваться по блокам данных каталога %s\n", dirent->name);
                return 0;
            }
        }

    }


    if (S_ISDIR(inode.i_mode))
    {   
        if(flag_error == true)
            printf("Невозможно открыть и проверить каталог %s из-за нарушения его целостности\n", dirent->name);
        else
            check_all_files(fs, dirent->inode, private_data->b_blocks);

    }

  
    return 0;
}


int test_block(ext2_filsys fs, blk_t *blocknr, int blockcnt, void *private) {

    int *not_found_blocks = (int *)private;
    errcode_t err;

    // Проверяем, отмечен ли используемый блок в карте блоков   
    if (!ext2fs_test_block_bitmap(fs->block_map, *blocknr)) {
        printf("Битый блок: %u\n", *blocknr);
        (*not_found_blocks)++;
    }
    return 0;
}

void close_fs(ext2_filsys* fs){
    errcode_t err = ext2fs_close(*fs);//Закрытие ФС
    if(err){
        fprintf(stderr, "Ошибка закрытия файловой системы: %s\n", error_message(err));
        exit(EXIT_FAILURE);
    }
}

int ext2fs_superblock_csum_my_verify(ext2_filsys fs, struct ext2_super_block* sb){ //1 в случае совпадения, 0 в случае несовпадения
    //return (fs->super->s_checksum == ext2fs_superblock_csum(fs, sb)); 
    if(fs->super->s_checksum != ext2fs_superblock_csum(fs, sb))
    {
        printf("Посчитано %lx, а в суперблоке хранится %lx\n", ext2fs_superblock_csum(fs, sb), fs->super->s_checksum);
        return 0;
    }
    else
        return 1;

}
