#include <stdio.h>
#include <unistd.h>  // Для функций lseek, close
#include <ext2fs/ext2_fs.h>
#include <ext2fs/ext2fs.h>
#include <stdlib.h>
#include <locale.h>
#include <sys/stat.h>  // Для макросов S_ISREG, S_ISDIR, S_ISLNK и т.д.
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

ext2_ino_t* mas_num_used_inodes;

bool flag_need_repair_inode_bitmap = false; //если обнаружится что файл по факту действующий, но в inode_bitmap он не отмечен, значит флаг стнет true и надо это исправить в функции void repair_bitmaps(ext2_filsys* fs)

bool auto_rapair_inode_bitmap = false;

bool auto_rapair_block_bitmap = false; 

void check_not_used_inodes_in_bitmap(ext2_filsys* fs, ext2_ino_t* mas_num_used_inodes);

bool error_with_csum = false;

bool error_with_celostnost = false;

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

bool input_flag_auto_repair();

void recalculation_сhecksums(char* device_path, ext2_filsys* fs);

void read_bits(FILE* device, char* buf, unsigned int num_block, int size);



int main(int argc, char **argv){

    setlocale(LC_ALL, "Rus");
    ext2_filsys fs = NULL; //дескриптор файловой системы | объявляешь переменную ext2_filsys fs - создаешь указатель на структуру struct struct_ext2_filsys

    if (argc != 2) {
        fprintf(stderr, "%s <device>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
     
    open_fs(&fs, argv[1]);

    printf("ШАГ1: Проверка целостноcти метаданных и файлов\n\n");
    check_gr_desc_and_inodes_and_data(&fs);

    if(ext2fs_test_ib_dirty(fs) || ext2fs_test_bb_dirty(fs)){
        printf("ШАГ 1.5: Пересчёт изменённой bitmap\n");
        recalculation_сhecksums(argv[1], &fs);
    }

    printf("ШАГ 2: Проверка контрольных сумм метаданных\n\n");
    check_csum_fs(&fs);

    print_filesystem_info(&fs); 

    close_fs(&fs);
   
    return 0;
}

//////////////////////////////////////

void recalculation_сhecksums(char *device_path, ext2_filsys* fs){

    write_bitmaps(fs);
    //Инициализирует начальное значение для вычисления контрольных сумм.
    ext2fs_init_csum_seed(*fs); 

    FILE* device = fopen(device_path, "rb");
    if (device == NULL) {
        perror("Ошибка открытия устройства или файла");
        return;
    }

    errcode_t err = 0;
    dgrp_t group;
    char* buf_inode_bitmap = (char*)malloc((*fs)->super->s_inodes_per_group/8);
    char* buf_block_bitmap = (char*)malloc((*fs)->super->s_blocks_per_group/8);

    for(dgrp_t i = 0; i < (*fs)->group_desc_count - 1; i++){
        struct ext4_group_desc *gdp = (struct ext4_group_desc *)ext2fs_group_desc(*fs, (*fs)->group_desc, i);

        read_bits(device, buf_inode_bitmap, (*gdp).bg_inode_bitmap, (*fs)->super->s_inodes_per_group);
        // Вычисляет и устанавливает контрольную сумму для карты инодов.
        if(ext2fs_inode_bitmap_csum_set(*fs, i, buf_inode_bitmap, (*fs)->super->s_inodes_per_group) != 0){ 
            printf("Ошибка вычисления и установки контрольной суммы inode_bitmap группы блоков %u\n", i);
            fprintf(stderr, "Ошибка: %s\n", error_message(err));
        }

        read_bits(device, buf_block_bitmap, (*gdp).bg_block_bitmap, (*fs)->super->s_blocks_per_group);
        // Вычисляет и устанавливает контрольную сумму для карты блоков.
        if(ext2fs_block_bitmap_csum_set(*fs, i, buf_block_bitmap, (*fs)->super->s_blocks_per_group) != 0)
            printf("Ошибка вычисления и установки контрольной суммы block_bitmap группы блоков %u\n", i);
        
        // Вычисляет и устанавливает контрольную сумму для блоков группы.
        ext2fs_group_desc_csum_set((*fs), i); 
   
    }
    write_bitmaps(fs);
    fclose(device);
    if(ext2fs_superblock_csum_set((*fs), (*fs)->super) != 0) // Вычисляет и устанавливает контрольную сумму для суперблока.
        printf("Ошибка вычисления и установки контрольной суммы суперблока\n");
    if (ext2fs_flush(*fs) != 0) { //Запись изменений на диск
        perror("Ошибка записи изменений на диск");
        exit(EXIT_FAILURE);
    }
}

void read_bits(FILE* device, char* buf, unsigned int num_block, int size){

    unsigned long long offset = (unsigned long long)num_block * BLOCK_SIZE;
    int bytes_to_read = size / 8; // Конвертируем количество битов в байты

    if (fseek(device, offset, SEEK_SET) != 0) {
        perror("Ошибка при перемещении к указанному блоку");
        exit(EXIT_FAILURE);
    }

    // Считываем данные из блока в буфер
    if (fread(buf, 1, bytes_to_read, device) != bytes_to_read) {
        perror("Ошибка при чтении блока");
        exit(EXIT_FAILURE);
    }

}

///////////////////...................ШАГ2......................///////////////

static __u32 ext2fs_superblock_csum(ext2_filsys fs, struct ext2_super_block *sb)
{
	int offset = offsetof(struct ext2_super_block, s_checksum); //смещение поля s_checksum в структуре данных ext2_super_block
	return ext2fs_crc32c_le(~0, (unsigned char *)sb, offset);
}

void check_csum_fs(ext2_filsys *fs){
    
    errcode_t err = 0;
    read_bitmaps(fs);

    if(!ext2fs_verify_csum_type(*fs, (*fs)->super)){
        printf("Тип контрольной суммы не поддерживается файловой системой\n");
        error_with_csum = true;
        return;
    }

    if(!ext2fs_superblock_csum_my_verify(*fs, (*fs)->super)){
        printf("Контрольная сумма суперблока не совпала с вычисленной контрольной суммой\n");
        error_with_csum = true;
        return;
    }

    if ((*fs)->inode_map == NULL) {
        printf("Ошибка: inode_map не инициализирован\n");
        error_with_csum = true;
        return;
    }
    if((*fs)->block_map == NULL){
        printf("Ошибка: inode_map не инициализирован\n");
        error_with_csum = true;
        return;
    }

    for(dgrp_t num_group = 0; num_group < (*fs)->group_desc_count; num_group++){

        if(!ext2fs_group_desc_csum_verify((*fs), num_group)){
            printf("Контрольная сумма группы блоков №%d не совпала с вычисленной\n", num_group); 
            error_with_csum = true;
            //заменяем на копию из другой группы блоков
            continue;
        }
        
        struct ext4_group_desc *gdp = (struct ext4_group_desc *)ext2fs_group_desc(*fs, (*fs)->group_desc, num_group);

        //контрольная сумма карты инодов в структуре данных ext4_group_desc
        __u32 provided_checksum_inode = gdp->bg_inode_bitmap_csum_lo; 
 
        __u32 inode_bitmap_checksum = ext2fs_inode_bitmap_checksum(*fs, num_group);

         if ((*fs)->super->s_desc_size >= EXT4_BG_INODE_BITMAP_CSUM_HI_END)
            provided_checksum_inode |= (__u32)gdp->bg_inode_bitmap_csum_hi << 16;
             // Сравниваем контрольные суммы
        if (inode_bitmap_checksum != provided_checksum_inode) {       //ext2fs_inode_bitmap_csum_verify - не работает почему-то
            printf("Контрольная сумма карты инодов группы блоков №%d не совпала с вычисленной\n", num_group);
            printf("Вычислено = %lx, указана в структуре данных = %ls\n", inode_bitmap_checksum, provided_checksum_inode);
            error_with_csum = true;
        }

        __u32 block_bitmap_checksum = ext2fs_block_bitmap_checksum(*fs, num_group);
                                        
        //контрольная сумма карты блоков в структуре данных ext4_group_desc
        __u32 provided_checsum_block = gdp->bg_block_bitmap_csum_lo; 

        if ((*fs)->super->s_desc_size >= EXT4_BG_BLOCK_BITMAP_CSUM_HI_LOCATION)
                 provided_checsum_block |= (__u32)gdp->bg_block_bitmap_csum_hi << 16;
            
        if (block_bitmap_checksum != provided_checsum_block) {  // ext2fs_block_bitmap_csum_verify - не работает почему-то
            printf("Контрольная сумма карты блоков группы блоков №%d не совпала с вычисленной\n", num_group);
            printf("Вычислено = %lx, указана в структуре данных = %ls\n", block_bitmap_checksum, provided_checsum_block);
            error_with_csum = true;
        }
    }

    if(!error_with_csum){
        printf("Ошибок не обнаружено\n\n");
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

    //Проверяем смонтировано ли устройство
    err = ext2fs_check_if_mounted(name_device, &mount_flags); 
    if (err) {
        fprintf(stderr, "Ошибка при проверке монтирования устройства: %s\n", error_message(err));
        exit(EXIT_FAILURE);
    }

    if (mount_flags) {
        printf("Устройство смонтировано. Размонтируйте его перед запуском утилиты\n");
        exit(EXIT_FAILURE);
    } 
   
   //Открытие ФС
    err = ext2fs_open(name_device, EXT2_FLAG_RW, 0, 0, unix_io_manager, fs); 
    if (err) {
        fprintf(stderr, "Ошибка открытия файловой системы: %s\n", error_message(err));
        exit(EXIT_FAILURE);
    }
    
    //считывание с диска растровых изображений
    read_bitmaps(fs); 
    
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

        if (!ext2fs_test_inode_bitmap(fs->inode_map, i) || i == 12)  //inode не действующий
            continue;

            struct ext2_inode inode;
            err = ext2fs_read_inode(fs, i, &inode);      
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
//////.........................ШАГ 1...............................///////

void check_not_used_inodes_in_bitmap(ext2_filsys* fs, ext2_ino_t* mas_num_used_inodes){
    //ext2fs_mark_inode_bitmap((*fs)->inode_map, 25); ///СОЗДАНИЕ ОШИБКИ НАМЕРЕННО!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    //write_bitmaps(fs);
    for(ext2_ino_t i = EXT2_FIRST_INO((*fs)->super); i < (*fs)->super->s_inodes_count; i++){
        if(mas_num_used_inodes[i] != 1 && i != 12 && ext2fs_test_inode_bitmap((*fs)->inode_map, i)){
            printf("Неиспользуемый inode %u помечен в inode_bitmap как используемый\n", i);
            error_with_celostnost = true; 
            flag_need_repair_inode_bitmap = true;
            if(auto_rapair_inode_bitmap){
                //Сбрасывает бит для указанного индексного узла в карте индексных узлов
                ext2fs_unmark_inode_bitmap((*fs)->inode_map, i); 
                //Пометить карты инодов как изменённую
                ext2fs_mark_ib_dirty(*fs);
            }
        }
    }
}

bool input_flag_auto_repair(){
    int ch;
    while(1){
        scanf("%d", &ch);
        switch (ch)
        {
        case 0:
            return false;
        case 1:
            return true;
        default:
            printf("Неверный символ\n");
            continue;
        }
    }
}

void check_gr_desc_and_inodes_and_data(ext2_filsys *fs){

    errcode_t err;
    int b_blocks = 0;

    //Эта функция проверяет структуру дескрипторов файловой системы на целостность.
    err = ext2fs_check_desc(*fs); 
    if (err) {
        fprintf(stderr, "Ошибка при проверке целостности дескрипторов файловой системы: %s\n", error_message(err));
        close_fs(fs);
        exit(EXIT_FAILURE);
    }
    
    printf("Исправлять автоматически ошибки возникшие с inode_bitmap?(1 - да: 0-нет): ");
    auto_rapair_inode_bitmap = input_flag_auto_repair();

    printf("Исправлять автоматически ошибки возникшие с block_bitmap?(1 - да: 0-нет): ");
    auto_rapair_block_bitmap = input_flag_auto_repair();

    mas_num_used_inodes = (ext2_ino_t*)calloc((*fs)->super->s_inodes_count, sizeof(ext2_ino_t));

    //EXT2_ROOT_INO - это макрос, представляющий номер inode корневого каталога в файловой системе Ext2
    check_all_files(fs, EXT2_ROOT_INO, &b_blocks); 
    
    if(b_blocks != 0)
    {   
        printf("Повреждение block_bitmap\n");
        printf("Блоки данных файлов, помеченные как неиспользуемые в bitmap: %d\n", b_blocks);
    }

    check_not_used_inodes_in_bitmap(fs, mas_num_used_inodes);

    if(!error_with_celostnost){
        printf("Ошибок не обнаружено\n\n");
    }
}

void check_all_files(ext2_filsys* fs, ext2_ino_t dir, int* b_blocks) {
    errcode_t err;
    int flags = 0;
    PrivateData private_data = {fs, b_blocks};
    mas_num_used_inodes[dir] = 1;

    //Итерация по записям каталога
    err = ext2fs_dir_iterate(*fs, dir, flags, NULL, process_dir_entry, &private_data);
    if(err){
        printf("Ошибка итерации по каталогу inode = %d\n", dir);
        error_with_celostnost = true;
        return;
    }
}

int process_dir_entry(struct ext2_dir_entry *dirent, int offset, int blocksize, char *buf, void *private) {

    mas_num_used_inodes[dirent->inode] = 1; 
     
    if(dirent->name[0] == '.')
        return 0;

    errcode_t err;
    struct ext2_inode inode;
    bool flag_error = false;
    PrivateData *private_data = (PrivateData *)private;
    ext2_filsys* fs = (*private_data).fs;


    err = ext2fs_read_inode(*fs, dirent->inode, &inode);
    if(err){
        printf("Ошибка считывания inode = %d файла %s\n", dirent->inode, dirent->name); 
        error_with_celostnost = true;
        return 0;
    }

    if (!ext2fs_test_inode_bitmap((*fs)->inode_map, dirent->inode)) {
        printf("Файловый inode = %d не отмечен в inode_bitmap как использующийся для файла %.*s", dirent->inode, dirent->name_len, dirent->name);
        flag_error = true;
        flag_need_repair_inode_bitmap = true;
        error_with_celostnost = true;
        if(auto_rapair_inode_bitmap){
            //Устанавливает бит для указанного индексного узла в карте индексных узлов
            ext2fs_mark_inode_bitmap((*fs)->inode_map, dirent->inode);
            //Пометить карты инодов как изменённую
            ext2fs_mark_ib_dirty(*fs);
        }
    }

    if (!S_ISCHR(inode.i_mode) && !S_ISBLK(inode.i_mode) && !S_ISLNK(inode.i_mode)) 
    {   
        //Проверка целостности блоков данных
        if(!ext2fs_inode_has_valid_blocks(&inode)){
            printf("Блоки данных файла %.*s не действительны\n", dirent->name_len, dirent->name);
            flag_error = true;
            error_with_celostnost = true;
        }
    }

    //Проверка контрольной суммы записи в каталоге
    if(!ext2fs_dirent_csum_verify((*fs), dirent->inode, dirent)){
        printf("Запись %s в каталоге имеет неправильную контрольную сумму\n", dirent->name);
        error_with_celostnost = true;
    }
   
    // Проверяем блоки данных inode
    if(S_ISDIR(inode.i_mode) || S_ISREG(inode.i_mode))
    {   
        //Итерация по блокам данных
        err = ext2fs_block_iterate(*fs, dirent->inode, BLOCK_FLAG_DATA_ONLY, NULL, test_block, private_data->b_blocks);
        if (err) {
            if(S_ISREG(inode.i_mode))
            {
                printf("Ошибка проверки (не удалось проитерироваться) блоков данных inode %d - файла %s: %s\n", dirent->inode, dirent->name);
                error_with_celostnost = true;
                return 0; 
            }
            if(S_ISDIR(inode.i_mode)){
                printf("Нарушение структуры каталогов, не удалось проитерироваться по блокам данных каталога %s\n", dirent->name);
                error_with_celostnost = true;
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
    //ext2fs_unmark_block_bitmap(fs->block_map, *blocknr); //ИСКУССТВЕННАЯ ОШИБКА ДЛЯ ПРОВЕРКИ
    //write_bitmaps(&fs);
    int *not_found_blocks = (int *)private;
    errcode_t err;
    // Проверяем, отмечен ли используемый блок в карте блоков   
    if (!ext2fs_test_block_bitmap(fs->block_map, *blocknr)) {
        printf("Действующий блок данных файла помеченный в карте блоков как неиспользуемый: %u\n", *blocknr);
        error_with_celostnost = true;
        (*not_found_blocks)++;
        if(auto_rapair_block_bitmap){
            //Устанавливает бит для указанного блока данных в карте блоков
            ext2fs_mark_block_bitmap(fs->block_map, *blocknr);
            //Пометить карты блоков как изменённую
            ext2fs_mark_bb_dirty(fs);
        }
    }
    return 0;
}

void close_fs(ext2_filsys* fs){
    //Закрытие ФС
    errcode_t err = ext2fs_close(*fs);
    if(err){
        fprintf(stderr, "Ошибка закрытия файловой системы: %s\n", error_message(err));
        exit(EXIT_FAILURE);
    }
}

int ext2fs_superblock_csum_my_verify(ext2_filsys fs, struct ext2_super_block* sb){ //1 в случае совпадения, 0 в случае несовпадения
    if(fs->super->s_checksum != ext2fs_superblock_csum(fs, sb))
    {
        printf("Посчитано %lx, а в суперблоке хранится %lx\n", ext2fs_superblock_csum(fs, sb), fs->super->s_checksum);
        return 0;
    }
    else
        return 1;

}
