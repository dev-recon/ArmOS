/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/fs/fat32_vfs.c
 * Layer: Kernel / VFS and filesystems
 *
 * Responsibilities:
 * - Provide filesystem-independent VFS operations.
 * - Implement persistent ext2/FAT32/procfs behavior.
 *
 * Notes:
 * - Keep file descriptor and inode ownership rules explicit.
 */

#include <kernel/vfs.h>
#include <kernel/fat32.h>
#include <kernel/memory.h>
#include <kernel/string.h>
#include <kernel/uart.h>
#include <kernel/kprintf.h>
#include <kernel/timer.h>

/* Forward declarations */
static inode_t* fat32_inode_lookup(inode_t* dir, const char* name);
static ssize_t fat32_file_read(file_t* file, void* buffer, size_t count);
static int fat32_file_open(inode_t* inode, file_t* file);
static int fat32_file_close(file_t* file);
static off_t fat32_file_lseek(file_t* file, off_t offset, int whence);
static int fat32_dir_readdir(file_t* file, dirent_t* dirent);
int fat32_inode_mkdir(inode_t* dir, const char* name, uint16_t mode);
int fat32_inode_rmdir(inode_t* dir, const char* name);
int fat32_add_dir_entry(uint32_t parent_cluster, fat32_dir_entry_t* new_entry);
int fat32_inode_unlink(inode_t* dir, const char* name);
int fat32_remove_dir_entry(uint32_t dir_cluster, const char* name);
bool fat32_directory_is_not_empty(uint32_t dir_cluster);

fat32_dir_entry_t* fat32_find_entry_by_cluster(uint32_t parent_cluster, uint32_t target_cluster);
fat32_dir_entry_t* fat32_search_recursive(uint32_t dir_cluster, inode_t* target_inode) ;
int fat32_write_dir_entry(fat32_dir_entry_t* entry) ;
int fat32_update_entry_on_disk(fat32_dir_entry_t* target_entry);
int fat32_update_entry_recursive(uint32_t dir_cluster, fat32_dir_entry_t* target_entry);

fat32_dir_entry_t* fat32_find_entry_in_all_dirs(inode_t* target_inode);
fat32_dir_entry_t* fat32_find_dir_entry_for_inode(inode_t* inode);
fat32_dir_entry_t* fat32_find_entry(uint32_t dir_cluster, const char* name);
fat32_dir_entry_t* fat32_create_dir_entry(const char* name, uint32_t cluster, mode_t mode);
void fat32_free_cluster(uint32_t cluster);

/* Convertir timestamp Unix vers date FAT32 */
uint16_t fat32_unix_to_fat_date(uint32_t unix_time);

/* Convertir timestamp Unix vers heure FAT32 */
uint16_t fat32_unix_to_fat_time(uint32_t unix_time);

/* Convertir date FAT vers timestamp Unix */
uint32_t fat32_fat_date_to_unix(uint16_t fat_date, uint16_t fat_time);
int fat32_convert_name_to_83(const char* long_name, char* short_name);

void mark_inode_dirty(inode_t* inode);

/* Synchroniser un inode vers le disque */
int sync_inode_to_disk(inode_t* inode);

/* Synchroniser tous les inodes dirty */
void sync_dirty_inodes(void);
void mark_fat_dirty(void) ;
int sync_fat_to_disk(void);

/* Calculer le nombre total de clusters */
uint32_t fat32_get_total_clusters(void);
uint32_t fat32_alloc_cluster(void);
uint32_t fat32_get_cluster_value(uint32_t cluster);
int fat32_set_cluster_value(uint32_t cluster, uint32_t value);
int fat32_write_cluster(uint32_t cluster, const char* data) ;
ssize_t fat32_file_write(file_t* file, const void* buffer, size_t count);

/* Convertir un numéro de cluster en secteur */
uint32_t fat32_cluster_to_sector(uint32_t cluster);

/* Obtenir le cluster depuis une entrée de répertoire */
uint32_t fat32_get_cluster_from_entry(fat32_dir_entry_t* entry);

/* Définir le cluster dans une entrée de répertoire */
void fat32_set_cluster_in_entry(fat32_dir_entry_t* entry, uint32_t cluster);

extern fat32_fs_t fat32_fs;

extern int blk_read_sectors(uint64_t lba, uint32_t count, void* buffer);
extern int blk_write_sectors(uint64_t lba, uint32_t count, void* buffer);
extern int blk_read_sector(uint64_t lba, void* buffer);
extern int blk_write_sector(uint64_t lba, void* buffer);

/* Operations tables */
file_operations_t fat32_file_ops = {
    .read = fat32_file_read,
    .write = fat32_file_write, 
    .open = fat32_file_open,
    .close = fat32_file_close,
    .lseek = fat32_file_lseek,
    .readdir = NULL
};

file_operations_t fat32_dir_ops = {
    .read = NULL,
    .write = NULL,
    .open = fat32_file_open,
    .close = fat32_file_close,
    .lseek = NULL,
    .readdir = fat32_dir_readdir
};

static int fat32_inode_rename(inode_t* old_dir, const char* old_name,
                              inode_t* new_dir, const char* new_name);

inode_operations_t fat32_inode_ops = {
    .lookup = fat32_inode_lookup,
    .create = NULL,
    .mkdir  = fat32_inode_mkdir,
    .unlink = fat32_inode_unlink,
    .rmdir  = fat32_inode_rmdir,
    .rename = fat32_inode_rename,
    .readlink = NULL,
};

static bool fat_dirty = false;
static uint32_t fat_dirty_first_sector = ~0u;
static uint32_t fat_dirty_last_sector;
static fat32_stats_t fat32_stats;

/* Liste des inodes modifiés */
#define MAX_DIRTY_INODES 64
#define FAT32_IO_BATCH_SECTORS 128u
static inode_t* dirty_inodes[MAX_DIRTY_INODES];
static int dirty_count = 0;


bool fat32_directory_is_not_empty(uint32_t dir_cluster) {
    uint32_t cluster = dir_cluster;
    
    while (cluster != 0 && cluster < FAT32_EOC) {
        uint32_t bytes_per_cluster = get_fat32_bytes_per_cluster();
        void* cluster_data = kmalloc(bytes_per_cluster);
        if (!cluster_data) {
            KERROR("[READDIR] Failed to allocate cluster buffer\n");
            return true;
        }
        
        /* Lire le cluster */
        if (fat32_read_cluster(cluster, cluster_data) < 0) {
            KERROR("[fat32_file_write] Failed to read cluster %u\n", cluster);
            kfree(cluster_data);
            return true;
        }
        
        fat32_dir_entry_t* entries = (fat32_dir_entry_t*)cluster_data;
        int entries_per_cluster = bytes_per_cluster / sizeof(fat32_dir_entry_t);
        
        for (int i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t* entry = &entries[i];
            
            if (entry->name[0] == FAT32_END_OF_ENTRIES) break;
            if (entry->name[0] == FAT32_DELETED_ENTRY) continue;
            if (IS_LONG_NAME(entry->attr)) continue;
            
            /* Ignorer . et .. */
            if (memcmp(entry->name, ".          ", 11) == 0 ||
                memcmp(entry->name, "..         ", 11) == 0) {
                continue;
            }
            
            /* Trouvé une entrée valide */
            kfree(cluster_data);
            return true;  /* Répertoire non vide */
        }
        
        kfree(cluster_data);
        cluster = fat32_get_next_cluster(cluster);
    }
    
    return false;  /* Répertoire vide */
}

void fat32_free_cluster_chain(uint32_t start_cluster) {
    if (start_cluster < 2 ) return;
    
    uint32_t cluster = start_cluster;
    
    //KDEBUG("Freeing cluster chain starting at %u\n", start_cluster);
    
    while (cluster != 0 && cluster < FAT32_EOC) {
        uint32_t next_cluster = fat32_get_next_cluster(cluster);
        
        /* Libérer le cluster actuel */
        fat32_set_cluster_value(cluster, FAT32_FREE_CLUSTER);
        
        //KDEBUG("Freed cluster %u\n", cluster);
        
        /* Passer au suivant */
        cluster = next_cluster;
    }
}

static int fat32_inode_rename(inode_t* old_dir, const char* old_name,
                              inode_t* new_dir, const char* new_name)
{
    fat32_dir_entry_t* old_entry = fat32_find_entry(old_dir->first_cluster, old_name);
    if (!old_entry) return -ENOENT;

    uint32_t cluster   = fat32_get_cluster_from_entry(old_entry);
    uint32_t file_size = old_entry->file_size;
    uint8_t  attr      = old_entry->attr;

    fat32_dir_entry_t* new_entry = fat32_create_dir_entry(new_name, cluster,
                                                           old_dir->mode & ~S_IFMT);
    if (!new_entry) {
        kfree(old_entry);
        return -ENOMEM;
    }
    new_entry->file_size = file_size;
    new_entry->attr      = attr;

    if (fat32_add_dir_entry(new_dir->first_cluster, new_entry) != 0) {
        kfree(old_entry);
        kfree(new_entry);
        return -EIO;
    }

    int ret = fat32_remove_dir_entry(old_dir->first_cluster, old_name);

    kfree(old_entry);
    kfree(new_entry);
    return ret;
}

int fat32_inode_unlink(inode_t* dir, const char* name) {
    /* Trouver l'entrée */
    fat32_dir_entry_t* entry = fat32_find_entry(dir->first_cluster, name);
    if (!entry) return -ENOENT;
    
    /* Vérifier que ce n'est pas un répertoire */
    if (entry->attr & FAT_ATTR_DIRECTORY) {
        kfree(entry);
        return -EISDIR;
    }
    
    /* Libérer la chaîne de clusters du fichier */
    uint32_t cluster = fat32_get_cluster_from_entry(entry);
    if (cluster != 0) {
        fat32_free_cluster_chain(cluster);  /* ← Utilisation ici */
    }
    
    /* Supprimer l'entrée du répertoire */
    int result = fat32_remove_dir_entry(dir->first_cluster, name);
    
    kfree(entry);
    return result;
}

int fat32_inode_rmdir(inode_t* dir, const char* name)
{
    if (!dir || !name) return -EINVAL;

    fat32_dir_entry_t* entry = fat32_find_entry(dir->first_cluster, name);
    if (!entry) return -ENOENT;

    if (!(entry->attr & FAT_ATTR_DIRECTORY)) {
        kfree(entry);
        return -ENOTDIR;
    }

    uint32_t cluster = fat32_get_cluster_from_entry(entry);
    if (fat32_directory_is_not_empty(cluster)) {
        kfree(entry);
        return -ENOTEMPTY;
    }

    int result = fat32_remove_dir_entry(dir->first_cluster, name);
    if (result == 0)
        fat32_free_cluster(cluster);

    kfree(entry);
    return result;
}

void fat32_free_cluster(uint32_t cluster) {
    if (cluster < 2 ) return;
    
    uint32_t total_clusters = fat32_get_total_clusters();
    if (cluster >= total_clusters) return;
    
    //KDEBUG("Freeing cluster %u", cluster);
    
    /* Marquer le cluster comme libre dans la FAT */
    fat32_set_cluster_value(cluster, FAT32_FREE_CLUSTER);
}

int fat32_init_directory(uint32_t dir_cluster, uint32_t parent_cluster) {
    char* cluster_data = kzalloc(get_fat32_bytes_per_cluster());
    if (!cluster_data) return -ENOMEM;
    
    fat32_dir_entry_t* entries = (fat32_dir_entry_t*)cluster_data;
    
    /* Créer l'entrée "." (répertoire courant) */
    memcpy(entries[0].name, ".          ", 11);
    entries[0].attr = FAT_ATTR_DIRECTORY;
    fat32_set_cluster_in_entry(&entries[0], dir_cluster);
    entries[0].file_size = 0;
    
    /* Créer l'entrée ".." (répertoire parent) */
    memcpy(entries[1].name, "..         ", 11);
    entries[1].attr = FAT_ATTR_DIRECTORY;
    fat32_set_cluster_in_entry(&entries[1], parent_cluster);
    entries[1].file_size = 0;
    
    /* Marquer la fin des entrées */
    entries[2].name[0] = FAT32_END_OF_ENTRIES;
    
    /* Écrire le cluster initialisé */
    int result = fat32_write_cluster(dir_cluster, cluster_data);
    kfree(cluster_data);
    
    return result;
}

int fat32_inode_mkdir(inode_t* dir, const char* name, uint16_t mode) {
    /* Vérifier que le nom n'existe pas déjà */
    if (fat32_find_entry(dir->first_cluster, name) != NULL) {
        return -EEXIST;
    }
    
    /* Allouer un cluster pour le nouveau répertoire */
    uint32_t new_cluster = fat32_alloc_cluster();
    if (new_cluster == 0) return -ENOSPC;
    
    /* Marquer la fin de chaîne */
    fat32_set_cluster_value(new_cluster, FAT32_EOC);
    
    /* Créer l'entrée de répertoire */
    fat32_dir_entry_t* entry = fat32_create_dir_entry(name, new_cluster, mode | S_IFDIR);
    if (!entry) {
        fat32_free_cluster(new_cluster);
        return -ENOMEM;
    }
    
    /* Ajouter au répertoire parent */
    if (fat32_add_dir_entry(dir->first_cluster, entry) != 0) {
        fat32_free_cluster(new_cluster);
        kfree(entry);
        return -EIO;
    }
    
    /* Initialiser le nouveau répertoire avec . et .. */
    if (fat32_init_directory(new_cluster, dir->first_cluster) != 0) {
        fat32_remove_dir_entry(dir->first_cluster, name);
        fat32_free_cluster(new_cluster);
        kfree(entry);
        return -EIO;
    }
    
    kfree(entry);
    return 0;
}

/**
 * Vérifier si un fichier existe déjà dans un répertoire
 */
int fat32_file_exists_in_dir(inode_t* dir_inode, const char* filename) {
    if (!dir_inode || !filename ) {
        return 0;
    }
    
    /* Vérifier que c'est un répertoire */
    if (!S_ISDIR(dir_inode->mode)) {
        return 0;
    }
    
    /* Vérifier que le cluster est valide */
    if (dir_inode->first_cluster == 0 || 
        dir_inode->first_cluster >= FAT32_EOC) {
        return 0;
    }
    
    /* Chercher le fichier dans le répertoire */
    fat32_dir_entry_t* entry = fat32_find_entry(dir_inode->first_cluster, filename);
    if (entry) {
        kfree(entry);
        return 1;  /* Trouvé */
    }
    
    return 0;  /* Pas trouvé */
}

int fat32_remove_dir_entry(uint32_t dir_cluster, const char* name) {
    if (!name ) return -EINVAL;
    
    /* Convertir le nom au format 8.3 pour la recherche */
    char search_name[11];
    if (fat32_convert_name_to_83(name, search_name) != 0) {
        return -EINVAL;
    }
    
    uint32_t cluster = dir_cluster;
    
    while (cluster != 0 && cluster < FAT32_EOC) {
        uint32_t bytes_per_cluster = get_fat32_bytes_per_cluster();
        void* cluster_data = kmalloc(bytes_per_cluster);
        if (!cluster_data) {
            KERROR("[READDIR] Failed to allocate cluster buffer\n");
            return -ENOMEM;
        }
        
        /* Lire le cluster */
        if (fat32_read_cluster(cluster, cluster_data) < 0) {
            KERROR("[fat32_file_write] Failed to read cluster %u\n", cluster);
            kfree(cluster_data);
            return -EIO;
        }
        
        fat32_dir_entry_t* entries = (fat32_dir_entry_t*)cluster_data;
        int entries_per_cluster = get_fat32_bytes_per_cluster() / sizeof(fat32_dir_entry_t);
        bool found = false;
        
        for (int i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t* entry = &entries[i];
            
            /* Fin des entrées */
            if (entry->name[0] == FAT32_END_OF_ENTRIES) break;
            
            /* Entrée déjà supprimée */
            if (entry->name[0] == FAT32_DELETED_ENTRY) continue;
            
            /* Entrée LFN */
            if (IS_LONG_NAME(entry->attr)) continue;
            
            /* Comparer les noms */
            if (memcmp(entry->name, search_name, 11) == 0) {
                /* Marquer comme supprimée */
                entry->name[0] = FAT32_DELETED_ENTRY;
                found = true;
                break;
            }
        }
        
        if (found) {
            /* Écrire le cluster modifié */
            int result = fat32_write_cluster(cluster, cluster_data);
            kfree(cluster_data);
            return result;
        }
        
        kfree(cluster_data);
        cluster = fat32_get_next_cluster(cluster);
    }
    
    return -ENOENT;  /* Entrée non trouvée */
}

/* Version simplifiée - noms courts seulement */
fat32_dir_entry_t* fat32_find_entry(uint32_t dir_cluster, const char* name) {
    if (!name ) return NULL;
    
    char search_name[11];
    fat32_convert_name_to_83(name, search_name);
    
    uint32_t cluster = dir_cluster;
    
    while (cluster != 0 && cluster < FAT32_EOC) {
        uint32_t bytes_per_cluster = get_fat32_bytes_per_cluster();
        void* cluster_data = kmalloc(bytes_per_cluster);
        if (!cluster_data) {
            KERROR("[READDIR] Failed to allocate cluster buffer\n");
            return NULL;
        }
        
        /* Lire le cluster */
        if (fat32_read_cluster(cluster, cluster_data) < 0) {
            KERROR("[fat32_file_write] Failed to read cluster %u\n", cluster);
            kfree(cluster_data);
            return NULL;
        }
        
        fat32_dir_entry_t* entries = (fat32_dir_entry_t*)cluster_data;
        int entries_per_cluster = get_fat32_bytes_per_cluster() / 32;  /* 32 = sizeof(fat32_dir_entry_t) */
        
        for (int i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t* entry = &entries[i];
            
            if (entry->name[0] == 0x00) break;           /* Fin */
            if (entry->name[0] == 0xE5) continue;        /* Supprimée */
            if ((entry->attr & 0x0F) == 0x0F) continue; /* LFN */
            
            if (memcmp(entry->name, search_name, 11) == 0) {
                fat32_dir_entry_t* result = kmalloc(sizeof(fat32_dir_entry_t));
                if (result) {
                    memcpy(result, entry, sizeof(fat32_dir_entry_t));
                }
                kfree(cluster_data);
                return result;
            }
        }
        
        kfree(cluster_data);
        cluster = fat32_get_next_cluster(cluster);
    }
    
    return NULL;
}

/* Conversion nom long vers 8.3 */
int fat32_convert_name_to_83(const char* long_name, char* short_name) {
    int name_len = 0, ext_len = 0;
    const char* ext_start = NULL;
    
    /* Initialiser avec des espaces */
    memset(short_name, ' ', 11);
    
    /* Trouver l'extension */
    ext_start = strrchr(long_name, '.');
    if (ext_start) {
        ext_start++; /* Passer le point */
        ext_len = strlen(ext_start);
        name_len = ext_start - long_name - 1;
    } else {
        name_len = strlen(long_name);
    }
    
    /* Limiter les longueurs */
    if (name_len > 8) name_len = 8;
    if (ext_len > 3) ext_len = 3;
    
    /* Copier le nom (convertir en majuscules) */
    for (int i = 0; i < name_len; i++) {
        char c = long_name[i];
        if (c >= 'a' && c <= 'z') c -= 32; /* Vers majuscule */
        short_name[i] = c;
    }
    
    /* Copier l'extension */
    if (ext_start) {
        for (int i = 0; i < ext_len; i++) {
            char c = ext_start[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            short_name[8 + i] = c;
        }
    }
    
    return 0;
}

/* Vérification des permissions */
bool fat32_inode_permission(inode_t* inode, int mask) {
    return inode_permission(inode, mask);
}

/* Initialiser un cluster de répertoire vide */
void fat32_init_empty_dir_cluster(uint32_t cluster) {
    char* cluster_data = kzalloc(fat32_fs.bytes_per_cluster);
    if (!cluster_data) return;
    
    /* Marquer comme fin des entrées */
    cluster_data[0] =  FAT32_END_OF_ENTRIES;
    
    fat32_write_cluster(cluster, cluster_data);
    kfree(cluster_data);
}

void fat32_init_new_inode(inode_t* inode, fat32_dir_entry_t* entry, 
                         inode_t* parent, mode_t mode) {
    (void) entry;
    /* create_inode() owns the VFS identity, reference count and hash link. */
    inode->mode = mode;
    inode->uid = current_uid();
    inode->gid = current_gid();
    inode->size = 0;
    inode->blocks = 0;
    inode->nlink = 1;
    
    /* Timestamps */
    uint32_t now = get_current_time();
    inode->atime = now;
    inode->mtime = now;
    inode->ctime = now;
    
    /* Champs spécifiques FAT32 */
    inode->first_cluster = 0;  /* Pas de cluster alloué pour l'instant */
    inode->parent_cluster = parent->first_cluster;
    
    /* Opérations */
    if (S_ISDIR(mode)) {
        inode->i_op = &fat32_inode_ops;
        inode->f_op = &fat32_dir_ops;
    } else {
        inode->i_op = &fat32_inode_ops;
        inode->f_op = &fat32_file_ops;
    }
    
}


int fat32_add_dir_entry(uint32_t parent_cluster, fat32_dir_entry_t* new_entry) {
    uint32_t cluster = parent_cluster;
    
    while (cluster != 0 && cluster < FAT32_EOC) {

        uint32_t bytes_per_cluster = get_fat32_bytes_per_cluster();
        void* cluster_data = kmalloc(bytes_per_cluster);
        if (!cluster_data) {
            KERROR("[READDIR] Failed to allocate cluster buffer\n");
            return -ENOMEM;
        }
        
        /* Lire le cluster */
        if (fat32_read_cluster(cluster, cluster_data) < 0) {
            KERROR("[fat32_file_write] Failed to read cluster %u\n", cluster);
            kfree(cluster_data);
            return -EIO;
        }
        
        fat32_dir_entry_t* entries = (fat32_dir_entry_t*)cluster_data;
        int entries_per_cluster = get_fat32_bytes_per_cluster() / sizeof(fat32_dir_entry_t);
        
        /* Chercher une entrée libre */
        for (int i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t* entry = &entries[i];
            
            /* Entree libre ou fin des entrees */
            if (entry->name[0] == FAT32_END_OF_ENTRIES || 
                entry->name[0] == FAT32_DELETED_ENTRY) {
                bool was_end = (entry->name[0] == FAT32_END_OF_ENTRIES);
                
                /* Copier la nouvelle entree */
                memcpy(entry, new_entry, sizeof(fat32_dir_entry_t));
                
                /* Si c'etait la fin, marquer la suivante comme fin. Il faut
                 * memoriser l'etat avant memcpy(), car entry contient
                 * maintenant le nouveau nom. */
                if (was_end && i + 1 < entries_per_cluster) {
                    entries[i + 1].name[0] = FAT32_END_OF_ENTRIES;
                }
                
                /* Ecrire le cluster modifie */
                int result = fat32_write_cluster(cluster, cluster_data);
                kfree(cluster_data);
                return result;
            }
        }
        
        kfree(cluster_data);
        
        /* Passer au cluster suivant */
        uint32_t next_cluster = fat32_get_next_cluster(cluster);
        if (next_cluster >= FAT32_EOC) {
            /* Besoin d'étendre le répertoire */
            next_cluster = fat32_alloc_cluster();
            if (next_cluster == 0) return -ENOSPC;
            
            fat32_set_cluster_value(cluster, next_cluster);
            fat32_set_cluster_value(next_cluster, FAT32_EOC);
            
            /* Initialiser le nouveau cluster */
            fat32_init_empty_dir_cluster(next_cluster);
        }
        cluster = next_cluster;
    }
    
    return -ENOSPC;
}


fat32_dir_entry_t* fat32_create_dir_entry(const char* name, uint32_t cluster, mode_t mode) {
    fat32_dir_entry_t* entry = kzalloc(sizeof(fat32_dir_entry_t));
    if (!entry) return NULL;
    
    /* Convertir le nom au format 8.3 */
    if (fat32_convert_name_to_83(name, entry->name) != 0) {
        kfree(entry);
        return NULL;
    }
    
    /* Définir les attributs */
    entry->attr = 0;
    if (S_ISDIR(mode)) {
        entry->attr |= FAT_ATTR_DIRECTORY;
    } else {
        entry->attr |= FAT_ATTR_ARCHIVE;
    }
    
    /* Définir le cluster */
    fat32_set_cluster_in_entry(entry, cluster);
    
    /* Taille du fichier (0 pour nouveau fichier) */
    entry->file_size = 0;
    
    /* Timestamps */
    uint32_t current_time = get_current_time();
    entry->create_time = fat32_unix_to_fat_time(current_time);
    entry->create_date = fat32_unix_to_fat_date(current_time);
    entry->last_access_date = entry->create_date;
    entry->write_time = entry->create_time;
    entry->write_date = entry->create_date;
    
    entry->create_time_tenth = 0;  /* Précision milliseconde */
    
    return entry;
}

inode_t* fat32_create_file(const char* parent_path, const char* filename, mode_t mode) {
    inode_t* parent_inode;
    inode_t* new_inode;
    fat32_dir_entry_t* new_entry;
    
    if (!parent_path || !filename) return NULL;
    
    /* Trouver l'inode du répertoire parent */
    parent_inode = path_lookup(parent_path);
    if (!parent_inode) {
        return NULL;  /* Répertoire parent inexistant */
    }
    
    /* Vérifier que le parent est bien un répertoire */
    if (!S_ISDIR(parent_inode->mode)) {
        put_inode(parent_inode);
        return NULL;
    }
    
    /* Vérifier les permissions d'écriture sur le parent */
    if (!fat32_inode_permission(parent_inode, MAY_WRITE)) {
        put_inode(parent_inode);
        return NULL;
    }
    
    /* Vérifier que le fichier n'existe pas déjà */
    if (fat32_find_entry(parent_inode->first_cluster, filename) != NULL) {
        put_inode(parent_inode);
        return NULL;  /* Fichier existe déjà */
    }
    
    /* Créer l'entrée de répertoire FAT32 */
    new_entry = fat32_create_dir_entry(filename, 0, mode);
    if (!new_entry) {
        put_inode(parent_inode);
        return NULL;
    }
    
    /* Ajouter l'entrée au répertoire parent */
    if (fat32_add_dir_entry(parent_inode->first_cluster, new_entry) != 0) {
        kfree(new_entry);
        put_inode(parent_inode);
        return NULL;
    }
    
    /* Créer le nouvel inode */
    new_inode = create_inode();
    if (!new_inode) {
        fat32_remove_dir_entry(parent_inode->first_cluster, filename);
        kfree(new_entry);
        put_inode(parent_inode);
        return NULL;
    }
    
    /* Initialiser l'inode */
    fat32_init_new_inode(new_inode, new_entry, parent_inode, mode);
    
    /* Ajouter à la liste des inodes */
    //add_inode_to_cache(new_inode);
    
    kfree(new_entry);
    put_inode(parent_inode);
    
    return new_inode;
}

/* Convertir un numéro de cluster en secteur */
uint32_t fat32_cluster_to_sector(uint32_t cluster) {
    if (cluster < 2) return 0;  /* Clusters 0 et 1 sont réservés */
    return fat32_fs.data_start_sector + 
           ((cluster - 2) * fat32_fs.sectors_per_cluster);
}

/* Obtenir le cluster depuis une entrée de répertoire */
uint32_t fat32_get_cluster_from_entry(fat32_dir_entry_t* entry) {
    return ((uint32_t)entry->first_cluster_hi << 16) | entry->first_cluster_lo;
}

/* Définir le cluster dans une entrée de répertoire */
void fat32_set_cluster_in_entry(fat32_dir_entry_t* entry, uint32_t cluster) {
    entry->first_cluster_lo = cluster & 0xFFFF;
    entry->first_cluster_hi = (cluster >> 16) & 0xFFFF;
}

fat32_dir_entry_t* fat32_find_entry_by_cluster(uint32_t parent_cluster, uint32_t target_cluster) {
    uint32_t cluster = parent_cluster;
    
    while (cluster != 0 && cluster < FAT32_EOC) {

        uint32_t bytes_per_cluster = get_fat32_bytes_per_cluster();
        void* cluster_data = kmalloc(bytes_per_cluster);
        if (!cluster_data) {
            KERROR("[READDIR] Failed to allocate cluster buffer\n");
            return NULL;
        }
        
        /* Lire le cluster */
        if (fat32_read_cluster(cluster, cluster_data) < 0) {
            KERROR("[fat32_file_write] Failed to read cluster %u\n", cluster);
            kfree(cluster_data);
            return NULL;
        }
        
        /* Parcourir les entrées dans ce cluster */
        fat32_dir_entry_t* entries = (fat32_dir_entry_t*)cluster_data;
        int entries_per_cluster = get_fat32_bytes_per_cluster() / sizeof(fat32_dir_entry_t);
        
        for (int i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t* entry = &entries[i];
            
            /* Ignorer les entrées supprimées ou vides */
            if (entry->name[0] == 0x00) break;  /* Fin des entrées */
            if (entry->name[0] == 0xE5) continue;  /* Entrée supprimée */
            if (entry->attr == FAT_ATTR_LFN) continue;  /* LFN */
            
            /* Vérifier si c'est le cluster recherché */
            uint32_t entry_cluster = fat32_get_cluster_from_entry(entry);
            if (entry_cluster == target_cluster) {
                /* Trouvé ! Copier l'entrée */
                fat32_dir_entry_t* result = kmalloc(sizeof(fat32_dir_entry_t));
                if (result) {
                    memcpy(result, entry, sizeof(fat32_dir_entry_t));
                }
                kfree(cluster_data);
                return result;
            }
        }
        
        kfree(cluster_data);
        
        /* Passer au cluster suivant du répertoire */
        cluster = fat32_get_next_cluster(cluster);
    }
    
    return NULL;
}


fat32_dir_entry_t* fat32_search_recursive(uint32_t dir_cluster, inode_t* target_inode) {
    uint32_t cluster = dir_cluster;
    
    while (cluster != 0 && cluster < FAT32_EOC) {
        uint32_t bytes_per_cluster = get_fat32_bytes_per_cluster();
        void* cluster_data = kmalloc(bytes_per_cluster);
        if (!cluster_data) {
            KERROR("[READDIR] Failed to allocate cluster buffer\n");
            return NULL;
        }
        
        /* Lire le cluster */
        if (fat32_read_cluster(cluster, cluster_data) < 0) {
            KERROR("[fat32_file_write] Failed to read cluster %u\n", cluster);
            kfree(cluster_data);
            return NULL;
        }
        
        fat32_dir_entry_t* entries = (fat32_dir_entry_t*)cluster_data;
        int entries_per_cluster = get_fat32_bytes_per_cluster() / sizeof(fat32_dir_entry_t);
        
        for (int i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t* entry = &entries[i];
            
            if (entry->name[0] == 0x00) break;
            if (entry->name[0] == 0xE5) continue;
            if (entry->attr == FAT_ATTR_LFN) continue;
            
            uint32_t entry_cluster = fat32_get_cluster_from_entry(entry);
            
            /* Si c'est le cluster recherché */
            if (entry_cluster == target_inode->first_cluster) {
                fat32_dir_entry_t* result = kmalloc(sizeof(fat32_dir_entry_t));
                if (result) {
                    memcpy(result, entry, sizeof(fat32_dir_entry_t));
                }
                kfree(cluster_data);
                return result;
            }
            
            /* Si c'est un répertoire, chercher récursivement */
            if ((entry->attr & FAT_ATTR_DIRECTORY) && 
                entry_cluster != dir_cluster) {  /* Éviter la récursion infinie avec . */
                
                fat32_dir_entry_t* recursive_result = 
                    fat32_search_recursive(entry_cluster, target_inode);
                if (recursive_result) {
                    kfree(cluster_data);
                    return recursive_result;
                }
            }
        }
        
        kfree(cluster_data);
        cluster = fat32_get_next_cluster(cluster);
    }
    
    return NULL;
}

int fat32_write_dir_entry(fat32_dir_entry_t* entry) {
    if (!entry ) return -EINVAL;
    
    /* Trouver où cette entrée est stockée sur le disque */
    //uint32_t entry_cluster = fat32_get_cluster_from_entry(entry);
    
    /* Chercher l'entrée dans son répertoire parent */
    /* Pour simplifier, on va chercher dans tous les répertoires */
    return fat32_update_entry_on_disk(entry);
}

int fat32_update_entry_on_disk(fat32_dir_entry_t* target_entry) {
    /* Parcourir tous les clusters de répertoires pour trouver cette entrée */
    return fat32_update_entry_recursive(fat32_fs.root_dir_cluster, target_entry);
}

int fat32_update_entry_recursive(uint32_t dir_cluster, fat32_dir_entry_t* target_entry) {
    uint32_t cluster = dir_cluster;
    uint32_t target_cluster = fat32_get_cluster_from_entry(target_entry);
    
    while (cluster != 0 && cluster < FAT32_EOC) {
        uint32_t bytes_per_cluster = get_fat32_bytes_per_cluster();
        void* cluster_data = kmalloc(bytes_per_cluster);
        if (!cluster_data) {
            KERROR("[READDIR] Failed to allocate cluster buffer\n");
            return -ENOMEM;
        }
        
        /* Lire le cluster */
        if (fat32_read_cluster(cluster, cluster_data) < 0) {
            KERROR("[fat32_file_write] Failed to read cluster %u\n", cluster);
            kfree(cluster_data);
            return -EIO;
        }
           
        fat32_dir_entry_t* entries = (fat32_dir_entry_t*)cluster_data;
        int entries_per_cluster = get_fat32_bytes_per_cluster() / sizeof(fat32_dir_entry_t);
        bool found = false;
        
        for (int i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t* entry = &entries[i];
            
            if (entry->name[0] == 0x00) break;
            if (entry->name[0] == 0xE5) continue;
            if (entry->attr == FAT_ATTR_LFN) continue;
            
            uint32_t entry_cluster = fat32_get_cluster_from_entry(entry);
            
            /* Si c'est l'entrée recherchée */
            if (entry_cluster == target_cluster && 
                memcmp(entry->name, target_entry->name, 11) == 0) {
                
                /* Mettre à jour l'entrée */
                memcpy(entry, target_entry, sizeof(fat32_dir_entry_t));
                found = true;
                break;
            }
        }
        
        if (found) {
            /* Écrire le cluster modifié */
            int result = fat32_write_cluster(cluster, cluster_data);
            kfree(cluster_data);
            return result;
        }
        
        kfree(cluster_data);
        cluster = fat32_get_next_cluster(cluster);
    }
    
    return -ENOENT;
}

fat32_dir_entry_t* fat32_find_entry_in_all_dirs(inode_t* target_inode) {
    /* Commencer par la racine */
    fat32_dir_entry_t* result = fat32_search_recursive(fat32_fs.root_dir_cluster, target_inode);
    return result;
}

fat32_dir_entry_t* fat32_find_dir_entry_for_inode(inode_t* inode) {
    if (!inode ) return NULL;
    
    /* Si c'est le répertoire racine */
    if (inode->first_cluster == fat32_fs.root_dir_cluster) {
        /* Créer une entrée factice pour la racine */
        fat32_dir_entry_t* root_entry = kzalloc(sizeof(fat32_dir_entry_t));
        if (!root_entry) return NULL;
        
        strncpy(root_entry->name, "           ", 11);  /* Nom vide pour racine */
        root_entry->attr = FAT_ATTR_DIRECTORY;
        fat32_set_cluster_in_entry(root_entry, fat32_fs.root_dir_cluster);
        root_entry->file_size = 0;
        
        return root_entry;
    }
    
    /* Chercher dans le répertoire parent */
    if (inode->parent_cluster == 0) {
        /* Pas de parent connu, chercher dans tous les répertoires */
        return fat32_find_entry_in_all_dirs(inode);
    }
    
    /* Chercher dans le répertoire parent spécifique */
    return fat32_find_entry_by_cluster(inode->parent_cluster, inode->first_cluster);
}


/* Convertir timestamp Unix vers date FAT32 */
uint16_t fat32_unix_to_fat_date(uint32_t unix_time) {
    datetime_t dt;
    unix_to_datetime(unix_time, &dt);
    
    /* Format FAT date: bits 15-9=année-1980, 8-5=mois, 4-0=jour */
    if (dt.year < 1980) {
        /* Date antérieure à 1980, utiliser la date minimale FAT */
        return 0x0021;  /* 1er janvier 1980 */
    }
    
    if (dt.year > 2107) {
        /* Date postérieure à 2107, utiliser la date maximale FAT */
        return 0xFF9F;  /* 31 décembre 2107 */
    }
    
    uint16_t fat_date = 0;
    fat_date |= ((dt.year - 1980) & 0x7F) << 9;  /* Année - 1980 (7 bits) */
    fat_date |= (dt.month & 0x0F) << 5;          /* Mois (4 bits) */
    fat_date |= (dt.day & 0x1F);                 /* Jour (5 bits) */
    
    return fat_date;
}

/* Convertir timestamp Unix vers heure FAT32 */
uint16_t fat32_unix_to_fat_time(uint32_t unix_time) {
    datetime_t dt;
    unix_to_datetime(unix_time, &dt);
    
    /* Format FAT time: bits 15-11=heures, 10-5=minutes, 4-0=secondes/2 */
    uint16_t fat_time = 0;
    fat_time |= (dt.hour & 0x1F) << 11;          /* Heures (5 bits) */
    fat_time |= (dt.minute & 0x3F) << 5;         /* Minutes (6 bits) */
    fat_time |= (dt.second / 2) & 0x1F;          /* Secondes/2 (5 bits) */
    
    return fat_time;
}

/* Convertir date FAT vers timestamp Unix */
uint32_t fat32_fat_date_to_unix(uint16_t fat_date, uint16_t fat_time) {
    /* Extraire les champs de la date FAT */
    int year = 1980 + ((fat_date >> 9) & 0x7F);
    int month = (fat_date >> 5) & 0x0F;
    int day = fat_date & 0x1F;
    
    /* Extraire les champs de l'heure FAT */
    int hour = (fat_time >> 11) & 0x1F;
    int minute = (fat_time >> 5) & 0x3F;
    int second = (fat_time & 0x1F) * 2;
    
    /* Valider les valeurs */
    if (year < 1980 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 59) {
        return 0;  /* Date invalide */
    }
    
    /* Calculer le nombre de jours depuis l'epoch Unix (1er janvier 1970) */
    uint32_t days = 0;
    
    /* Ajouter les années complètes depuis 1970 */
    for (int y = 1970; y < year; y++) {
        days += is_leap_year(y) ? 366 : 365;
    }
    
    /* Ajouter les mois complets de l'année courante */
    for (int m = 1; m < month; m++) {
        days += get_days_in_month(m, year);
    }
    
    /* Ajouter les jours */
    days += day - 1;  /* -1 car on compte depuis le 1er */
    
    /* Convertir en secondes et ajouter l'heure */
    uint32_t unix_time = days * 86400 + hour * 3600 + minute * 60 + second;
    
    return unix_time;
}


void mark_inode_dirty(inode_t* inode) {
    if (!inode) return;
    
    /* Mettre à jour le timestamp */
    inode->mtime = get_current_time();
    
    /* Vérifier si déjà dans la liste */
    for (int i = 0; i < dirty_count; i++) {
        if (dirty_inodes[i] == inode) {
            return;  /* Déjà marqué comme dirty */
        }
    }
    
    /* Ajouter à la liste si pas pleine */
    if (dirty_count < MAX_DIRTY_INODES) {
        dirty_inodes[dirty_count++] = inode;
    } else {
        /* Liste pleine, synchroniser immédiatement */
        sync_inode_to_disk(inode);
    }
}

/* Synchroniser un inode vers le disque */
int sync_inode_to_disk(inode_t* inode) {
    /* Trouver l'entrée de répertoire correspondante */
    fat32_dir_entry_t* entry = fat32_find_dir_entry_for_inode(inode);
    if (!entry) return -ENOENT;
    
    /* Mettre à jour les champs de l'entrée */
    entry->file_size = inode->size;
    fat32_set_cluster_in_entry(entry, inode->first_cluster);
    entry->write_date = fat32_unix_to_fat_date(inode->mtime);
    entry->write_time = fat32_unix_to_fat_time(inode->mtime);
    
    /* Écrire l'entrée mise à jour */
    int result = fat32_write_dir_entry(entry);
    kfree(entry);
    
    return result;
}

/* Synchroniser tous les inodes dirty */
void sync_dirty_inodes(void) {
    //KDEBUG("SYNCING %u INODES TO DISK....\n", dirty_count);
    for (int i = 0; i < dirty_count; i++) {
        sync_inode_to_disk(dirty_inodes[i]);
        dirty_inodes[i] = NULL;
    }
    dirty_count = 0;
}

bool is_dirty_inodes(void){
    return dirty_count > 0;
}


void mark_fat_dirty(void) {
    fat_dirty = true;
}

static void mark_fat_sector_dirty(uint32_t relative_sector)
{
    fat_dirty = true;
    if (relative_sector < fat_dirty_first_sector)
        fat_dirty_first_sector = relative_sector;
    if (fat_dirty_first_sector == ~0u ||
        relative_sector > fat_dirty_last_sector)
        fat_dirty_last_sector = relative_sector;
}

bool is_fat_dirty(void){
    return fat_dirty;
}

void fat32_reset_stats(void)
{
    memset(&fat32_stats, 0, sizeof(fat32_stats));
}

void fat32_note_cluster_reads(uint32_t count)
{
    fat32_stats.cluster_reads += count;
}

void fat32_get_stats(fat32_stats_t* out)
{
    if (!out)
        return;
    *out = fat32_stats;
    out->mounted = fat32_fs.mounted ? 1u : 0u;
    out->dirty = fat_dirty ? 1u : 0u;
    out->bytes_per_cluster = fat32_fs.bytes_per_cluster;
    out->dirty_first_sector = fat_dirty_first_sector;
    out->dirty_last_sector = fat_dirty_last_sector;
}

int sync_fat_to_disk(void) {
    uint32_t first_sector;
    uint32_t sector_count;
    uint32_t fat2_start_sector;
    char* fat_data;

    if (!fat_dirty ) {
        return 0;
    }

    if (!fat32_fs.fat_table)
        return -EIO;

    /*
     * Cluster updates normally identify the exact FAT sector range. Keep the
     * full-table fallback for older call sites that only mark the FAT dirty.
     */
    if (fat_dirty_first_sector == ~0u) {
        first_sector = 0;
        sector_count = fat32_fs.boot_sector.fat_size_32;
    } else {
        first_sector = fat_dirty_first_sector;
        sector_count = fat_dirty_last_sector - fat_dirty_first_sector + 1u;
    }

    fat_data = (char*)fat32_fs.fat_table + first_sector * FAT32_SECTOR_SIZE;
    if (blk_write_sectors(fat32_fs.fat_start_sector + first_sector,
                          sector_count, fat_data) != 0)
        return -EIO;

    fat2_start_sector = fat32_fs.fat_start_sector +
                        fat32_fs.boot_sector.fat_size_32;
    if (fat32_fs.boot_sector.num_fats > 1 &&
        blk_write_sectors(fat2_start_sector + first_sector,
                          sector_count, fat_data) != 0)
        return -EIO;

    fat32_stats.fat_syncs++;
    fat32_stats.fat_sync_sectors += sector_count *
        (fat32_fs.boot_sector.num_fats > 1 ? 2u : 1u);
    fat_dirty = false;
    fat_dirty_first_sector = ~0u;
    fat_dirty_last_sector = 0;
    return 0;
}

/* Calculer le nombre total de clusters */
uint32_t fat32_get_total_clusters(void) {
    
    uint32_t total_sectors = fat32_fs.boot_sector.total_sectors_32;
    if (total_sectors == 0) {
        total_sectors = fat32_fs.boot_sector.total_sectors_16;
    }
    
    uint32_t data_start_rel = fat32_fs.data_start_sector - (uint32_t)fat32_fs.lba_start;
    uint32_t data_sectors = total_sectors - data_start_rel;
    return data_sectors / fat32_fs.sectors_per_cluster;
}

int fat32_statfs(struct statfs* st)
{
    uint32_t total_clusters;
    uint32_t data_clusters;
    uint32_t free_clusters = 0;

    if (!st)
        return -EINVAL;
    if (!fat32_fs.mounted || !fat32_fs.fat_table)
        return -ENODEV;

    total_clusters = fat32_get_total_clusters();
    data_clusters = total_clusters > 2 ? total_clusters - 2 : 0;

    for (uint32_t cluster = 2; cluster < total_clusters; cluster++) {
        if (fat32_get_cluster_value(cluster) == FAT32_FREE_CLUSTER)
            free_clusters++;
    }

    memset(st, 0, sizeof(*st));
    st->f_type = 0x4D44; /* MSDOS/FAT */
    st->f_bsize = fat32_fs.bytes_per_cluster;
    st->f_blocks = data_clusters;
    st->f_bfree = free_clusters;
    st->f_bavail = free_clusters;
    st->f_namelen = FAT32_MAX_FILENAME;
    st->f_frsize = fat32_fs.bytes_per_cluster;
    return 0;
}

uint32_t fat32_alloc_cluster(void) {
    if (!fat32_fs.mounted) {
        KERROR("fat32_alloc_cluster: FS not mounted\n");
        return 0;
    }
    
    static uint32_t last_allocated = 2;
    uint32_t total_clusters = fat32_get_total_clusters();
    
    //KDEBUG("fat32_alloc_cluster: searching from cluster %u to %u\n", 
    //       last_allocated, total_clusters);
    
    /* Chercher un cluster libre à partir du dernier alloué */
    for (uint32_t cluster = last_allocated; cluster < total_clusters; cluster++) {
        uint32_t value = fat32_get_cluster_value(cluster);
        if (value == FAT32_FREE_CLUSTER) {
            //KDEBUG("fat32_alloc_cluster: found free cluster %u (value was 0x%X)\n", 
            //       cluster, value);
            
            fat32_set_cluster_value(cluster, FAT32_EOC);
            
            /* Vérifier que l'écriture a réussi */
            //uint32_t verify = fat32_get_cluster_value(cluster);
            //KDEBUG("fat32_alloc_cluster: cluster %u now has value 0x%X (expected 0x%X)\n",
            //       cluster, verify, FAT32_EOC);
            
            mark_fat_dirty();
            last_allocated = cluster + 1;
            return cluster;
        }
    }
    
    //KDEBUG("fat32_alloc_cluster: no free cluster from %u to %u, wrapping around\n",
    //       last_allocated, total_clusters);
    
    /* Si pas trouvé, chercher depuis le début */
    for (uint32_t cluster = 2; cluster < last_allocated; cluster++) {
        uint32_t value = fat32_get_cluster_value(cluster);
        if (value == FAT32_FREE_CLUSTER) {
            //KDEBUG("fat32_alloc_cluster: found free cluster %u in wrap-around\n", cluster);
            
            fat32_set_cluster_value(cluster, FAT32_EOC);
            //uint32_t verify = fat32_get_cluster_value(cluster);
            //KDEBUG("fat32_alloc_cluster: cluster %u now has value 0x%X\n", cluster, verify);
            
            mark_fat_dirty();
            last_allocated = cluster + 1;
            return cluster;
        }
    }
    
    //KERROR("fat32_alloc_cluster: NO FREE CLUSTERS! Total=%u, last=%u\n",
    //       total_clusters, last_allocated);
    
    /* Debug: afficher quelques valeurs de la FAT */
/*     KDEBUG("FAT sample: cluster 2=0x%X, 3=0x%X, 4=0x%X, 527=0x%X, 528=0x%X\n",
           fat32_get_cluster_value(2),
           fat32_get_cluster_value(3),
           fat32_get_cluster_value(4),
           fat32_get_cluster_value(527),
           fat32_get_cluster_value(528)); */
    
    return 0;
}


/* uint32_t fat32_alloc_cluster2(void) {
    if (!fat32_fs.mounted) return 0;
    
    static uint32_t last_allocated = 2;
    uint32_t total_clusters = fat32_get_total_clusters();
    
    // Chercher un cluster libre à partir du dernier alloué 
    for (uint32_t cluster = last_allocated; cluster < total_clusters; cluster++) {
        uint32_t value = fat32_get_cluster_value(cluster);
        if (value == FAT32_FREE_CLUSTER) {
            fat32_set_cluster_value(cluster, FAT32_EOC);
            last_allocated = cluster + 1;
            return cluster;
        }
    }
    
    // Si pas trouvé, chercher depuis le début 
    for (uint32_t cluster = 2; cluster < last_allocated; cluster++) {
        uint32_t value = fat32_get_cluster_value(cluster);
        if (value == FAT32_FREE_CLUSTER) {
            fat32_set_cluster_value(cluster, FAT32_EOC);
            last_allocated = cluster + 1;
            return cluster;
        }
    }
    
    return 0;  // Pas de cluster libre 
} */

uint32_t fat32_get_cluster_value(uint32_t cluster) {
    if (cluster < 2) return 0;
    
    uint32_t total_clusters = fat32_get_total_clusters();
    if (cluster >= total_clusters) return 0;
    
    /* Si la FAT est en mémoire */
    if (fat32_fs.fat_table) {
        return fat32_fs.fat_table[cluster] & 0x0FFFFFFF;
    }
    
    /* Sinon, lire depuis le disque */
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat32_fs.fat_start_sector + 
                         (fat_offset / 512);  /* Supposer 512 bytes/secteur */
    uint32_t sector_offset = fat_offset % 512;
    
    static char fat_cache[512];
    static uint32_t cached_sector = 0xFFFFFFFF;
    
    if (fat_sector != cached_sector) {
        if (blk_read_sector(fat_sector, fat_cache) != 0) {
            return 0;
        }
        cached_sector = fat_sector;
    }
    
    uint32_t value = *(uint32_t*)(fat_cache + sector_offset);
    return value & 0x0FFFFFFF;
}


int fat32_set_cluster_value(uint32_t cluster, uint32_t value) {
    if (cluster < 2) return -EINVAL;
    
    uint32_t total_clusters = fat32_get_total_clusters();
    if (cluster >= total_clusters) return -EINVAL;
    
    /* Si la FAT est en mémoire */
    if (fat32_fs.fat_table) {
        uint32_t relative_sector = (cluster * sizeof(uint32_t)) /
                                   FAT32_SECTOR_SIZE;

        fat32_fs.fat_table[cluster] = 
            (fat32_fs.fat_table[cluster] & 0xF0000000) | (value & 0x0FFFFFFF);
        
        /* Marquer comme dirty pour synchronisation ultérieure */
        mark_fat_sector_dirty(relative_sector);
        return 0;
    }
    
    /* Sinon, écrire sur le disque */
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat32_fs.fat_start_sector + (fat_offset / 512);
    uint32_t sector_offset = fat_offset % 512;
    
    static char fat_cache[512];
    static uint32_t cached_sector = 0xFFFFFFFF;
    
    /* Lire le secteur si pas en cache */
    if (fat_sector != cached_sector) {
        if (blk_read_sector(fat_sector, fat_cache) != 0) {
            return -EIO;
        }
        cached_sector = fat_sector;
    }
    
    /* Modifier la valeur */
    uint32_t* fat_entry = (uint32_t*)(fat_cache + sector_offset);
    *fat_entry = (*fat_entry & 0xF0000000) | (value & 0x0FFFFFFF);
    
    /* Écrire le secteur modifié */
    if (blk_write_sector(fat_sector, fat_cache) != 0) {
        return -EIO;
    }

    /* AJOUT : Écrire aussi dans FAT2 (copie de sauvegarde) */
    uint32_t fat2_sector = fat_sector + fat32_fs.boot_sector.fat_size_32;
    if (blk_write_sector(fat2_sector, fat_cache) != 0) {
        KDEBUG("ERROR: Failed to sync FAT2 at sector %u", fat2_sector);
        return -EIO;
    }
    
    //KDEBUG("Updated cluster %u in both FAT1 (sector %u) and FAT2 (sector %u)", 
    //       cluster, fat_sector, fat2_sector);
    
    return 0;
}


int fat32_write_cluster(uint32_t cluster, const char* data) {
    if (cluster < 2 || cluster >= fat32_get_total_clusters()) {
        return -EINVAL;
    }
    
    uint32_t start_sector = fat32_cluster_to_sector(cluster);

    /* Debug du contenu avant écriture secteur */
    //KDEBUG("fat32_write_cluster: data content=%.64s\n", data);


    //KDEBUG("fat32_write_cluster: cluster=%u -> start_sector=%u\n", cluster, start_sector);
    //KDEBUG("fat32_write_cluster: data_start_sector=%u\n", fat32_fs.data_start_sector);
    //KDEBUG("fat32_write_cluster: sectors_per_cluster=%u\n", fat32_fs.sectors_per_cluster);

    /* Vérifier que le calcul est correct */
    uint32_t expected_sector = fat32_fs.data_start_sector + ((cluster - 2) * fat32_fs.sectors_per_cluster);
    //KDEBUG("fat32_write_cluster: expected_sector=%u, calculated=%u\n", expected_sector, start_sector);
    
    if (start_sector != expected_sector) {
        KDEBUG("ERROR: Sector calculation mismatch!\n");
        return -EIO;
    }

    
    if (blk_write_sectors(start_sector, get_fat32_sectors_per_cluster(),
                          (void*)data) != 0)
        return -EIO;
    fat32_stats.cluster_writes++;
    return 0;
}

static int fat32_write_contiguous_clusters(uint32_t first_cluster,
                                           uint32_t cluster_count,
                                           const void* data)
{
    uint32_t sectors_per_cluster = get_fat32_sectors_per_cluster();
    uint32_t sector_count;

    if (!data || cluster_count == 0 || sectors_per_cluster == 0)
        return -EINVAL;
    if (cluster_count > FAT32_IO_BATCH_SECTORS / sectors_per_cluster)
        return -EINVAL;

    sector_count = cluster_count * sectors_per_cluster;
    if (blk_write_sectors(fat32_cluster_to_sector(first_cluster),
                          sector_count, (void*)data) != 0)
        return -EIO;
    fat32_stats.cluster_writes += cluster_count;
    return 0;
}

static uint32_t fat32_prepare_contiguous_write_run(uint32_t first_cluster,
                                                   uint32_t wanted,
                                                   inode_t* inode,
                                                   uint32_t* last_cluster)
{
    uint32_t current = first_cluster;
    uint32_t count = 1;

    while (count < wanted) {
        uint32_t next = fat32_get_next_cluster(current);

        if (next >= FAT32_EOC) {
            next = fat32_alloc_cluster();
            if (next == 0)
                break;
            fat32_set_cluster_value(current, next);
            inode->blocks += fat32_fs.sectors_per_cluster;
        }
        if (next != current + 1u)
            break;
        current = next;
        count++;
    }

    *last_cluster = current;
    return count;
}

int fat32_update_file_size_in_dir(const char* filename, uint32_t parent_cluster, uint32_t new_size) {
    if (!filename ) return -EINVAL;
    
    char search_name[11];
    if (fat32_convert_name_to_83(filename, search_name) != 0) {
        return -EINVAL;
    }
    
    //KDEBUG("Updating size of '%.11s' to %u bytes\n", search_name, new_size);
    
    uint32_t cluster = parent_cluster;
    
    while (cluster != 0 && cluster < FAT32_EOC) {
        uint32_t bytes_per_cluster = get_fat32_bytes_per_cluster();
        void* cluster_data = kmalloc(bytes_per_cluster);
        if (!cluster_data) {
            KERROR("[fat32_file_write] Failed to allocate cluster buffer\n");
            return -ENOMEM;
        }

        /* Lire le cluster */
        if (fat32_read_cluster(cluster, cluster_data) < 0) {
            KERROR("[fat32_file_write] Failed to read cluster %u\n", cluster);
            kfree(cluster_data);
            return -EIO;
        }
        
        fat32_dir_entry_t* entries = (fat32_dir_entry_t*)cluster_data;
        int entries_per_cluster = get_fat32_bytes_per_cluster() / sizeof(fat32_dir_entry_t);
        bool found = false;
        
        for (int i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t* entry = &entries[i];
            
            if (entry->name[0] == FAT32_END_OF_ENTRIES) break;
            if (entry->name[0] == FAT32_DELETED_ENTRY) continue;
            if (IS_LONG_NAME(entry->attr)) continue;
            if (entry->attr & FAT_ATTR_VOLUME_ID) continue;
            
            if (memcmp(entry->name, search_name, 11) == 0) {
                //KDEBUG("Found entry '%.11s': old_size=%u, new_size=%u\n", 
                //       entry->name, entry->file_size, new_size);
                entry->file_size = new_size;
                found = true;
                break;
            }
        }
        
        if (found) {
            int result = fat32_write_cluster(cluster, cluster_data);
            kfree(cluster_data);
            //KDEBUG("Updated directory entry size, write result=%d\n", result);
            return result;
        }
        
        kfree(cluster_data);
        cluster = fat32_get_next_cluster(cluster);
    }
    
    //KDEBUG("ERROR: Could not find file '%.11s' to update size\n", search_name);
    return -ENOENT;
}

int fat32_update_file_cluster_in_parent(inode_t* file_inode, uint32_t new_cluster) {
    if (!file_inode ) return -EINVAL;
    
    /* Chercher dans le répertoire parent */
    uint32_t parent_cluster = file_inode->parent_cluster;
    if (parent_cluster == 0) {
        KERROR("[ERROR] No parent cluster for inode %u\n", file_inode->ino);
        return -EINVAL;
    }
    
    //KDEBUG("[DEBUG] Searching in parent cluster %u\n", parent_cluster);
    
    uint32_t cluster = parent_cluster;
    
    while (cluster != 0 && cluster < FAT32_EOC) {
        uint32_t bytes_per_cluster = get_fat32_bytes_per_cluster();
        void* cluster_data = kmalloc(bytes_per_cluster);
        if (!cluster_data) {
            KERROR("[fat32_file_write] Failed to allocate cluster buffer\n");
            return -ENOMEM;
        }

        /* Lire le cluster */
        if (fat32_read_cluster(cluster, cluster_data) < 0) {
            KERROR("[fat32_file_write] Failed to read cluster %u\n", cluster);
            kfree(cluster_data);
            return -EIO;
        }
        
        fat32_dir_entry_t* entries = (fat32_dir_entry_t*)cluster_data;
        int entries_per_cluster = get_fat32_bytes_per_cluster() / sizeof(fat32_dir_entry_t);
        bool found = false;
        
        for (int i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t* entry = &entries[i];
            
            if (entry->name[0] == FAT32_END_OF_ENTRIES) break;
            if (entry->name[0] == FAT32_DELETED_ENTRY) continue;
            if (IS_LONG_NAME(entry->attr)) continue;
            
            /* Vérifier si c'est notre fichier (cluster 0 initialement) */
            uint32_t entry_cluster = fat32_get_cluster_from_entry(entry);
            if (entry_cluster == 0 && entry->file_size == 0) {
                /* C'est probablement notre fichier - mettre à jour */
                //KDEBUG("[DEBUG] Found entry to update: %.11s\n", entry->name);
                
                fat32_set_cluster_in_entry(entry, new_cluster);
                found = true;
                break;
            }
        }
        
        if (found) {
            /* Écrire le cluster modifié */
            int result = fat32_write_cluster(cluster, cluster_data);
            kfree(cluster_data);
            //KDEBUG("[DEBUG] Directory cluster %u updated, result=%d\n", cluster, result);
            return result;
        }
        
        kfree(cluster_data);
        cluster = fat32_get_next_cluster(cluster);
    }
    
    //KERROR("[ERROR] Could not find directory entry to update\n");
    return -ENOENT;
}

int fat32_update_file_by_name(const char* filename, uint32_t parent_cluster, uint32_t new_cluster) {
    if (!filename ) return -EINVAL;
    
    /* Convertir le nom au format 8.3 */
    char search_name[11];
    if (fat32_convert_name_to_83(filename, search_name) != 0) {
        return -EINVAL;
    }
    
    //KDEBUG("[DEBUG] Looking for file '%.11s' (from '%s')\n", search_name, filename);
    
    uint32_t cluster = parent_cluster;
    
    while (cluster != 0 && cluster < FAT32_EOC) {
        uint32_t bytes_per_cluster = get_fat32_bytes_per_cluster();
        void* cluster_data = kmalloc(bytes_per_cluster);
        if (!cluster_data) {
            KERROR("[fat32_file_write] Failed to allocate cluster buffer\n");
            return -ENOMEM;
        }

        /* Lire le cluster */
        if (fat32_read_cluster(cluster, cluster_data) < 0) {
            KERROR("[fat32_file_write] Failed to read cluster %u\n", cluster);
            kfree(cluster_data);
            return -EIO;
        }
        
        fat32_dir_entry_t* entries = (fat32_dir_entry_t*)cluster_data;
        int entries_per_cluster = get_fat32_bytes_per_cluster() / sizeof(fat32_dir_entry_t);
        bool found = false;
        
        for (int i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t* entry = &entries[i];
            
            if (entry->name[0] == FAT32_END_OF_ENTRIES) break;
            if (entry->name[0] == FAT32_DELETED_ENTRY) continue;
            if (IS_LONG_NAME(entry->attr)) continue;
            
            /* Ignorer les entrées de volume */
            if (entry->attr & FAT_ATTR_VOLUME_ID) continue;
            
            //KDEBUG("[DEBUG] Comparing '%.11s' with '%.11s'\n", entry->name, search_name);
            
            /* Comparer les noms */
            if (memcmp(entry->name, search_name, 11) == 0) {
                //KDEBUG("[DEBUG] MATCH! Updating '%.11s' cluster to %u\n", entry->name, new_cluster);
                
                fat32_set_cluster_in_entry(entry, new_cluster);
                found = true;
                break;
            }
        }
        
        if (found) {
            int result = fat32_write_cluster(cluster, cluster_data);
            kfree(cluster_data);
            return result;
        }
        
        kfree(cluster_data);
        cluster = fat32_get_next_cluster(cluster);
    }
    
    //KERROR("[ERROR] File '%.11s' not found in directory\n", search_name);
    return -ENOENT;
}


ssize_t fat32_file_write(file_t* file, const void* buffer, size_t count) {
    inode_t* inode = file->inode;
    const char* buf = (const char*)buffer;
    
    if (!buffer || count == 0) return 0;
    if (!inode || !file) return -EINVAL;
    if(!can_write(file)) return -EPERM;

    size_t bytes_written = 0;
    uint32_t current_offset = file->offset;
    uint32_t cluster = inode->first_cluster;
    void* cluster_data = NULL;
    
    /* CORRECTION: Taille du cluster, pas du secteur */
    uint32_t cluster_size = get_fat32_bytes_per_cluster();  // Pas 512 !
    
    //KDEBUG("fat32_file_write: offset=%u, cluster=%u, count=%u, cluster_size=%u\n", 
    //       current_offset, cluster, count, cluster_size);

    /* Si le fichier est vide, allouer le premier cluster */
    if (cluster == 0) {
        cluster = fat32_alloc_cluster();
        if (cluster == 0) return -ENOSPC;
        
        inode->first_cluster = cluster;
        inode->blocks = fat32_fs.sectors_per_cluster; 
        mark_inode_dirty(inode);
        
        fat32_update_file_by_name(file->name, inode->parent_cluster, cluster);
    }
    
    /* Naviguer jusqu'au cluster de départ */
    uint32_t cluster_offset = current_offset / cluster_size;
    
    for (uint32_t i = 0; i < cluster_offset && cluster < FAT32_EOC; i++) {
        uint32_t next = fat32_get_next_cluster(cluster);
        
        if (next >= FAT32_EOC) {
            next = fat32_alloc_cluster();
            if (next == 0) return bytes_written ? (ssize_t)bytes_written : -ENOSPC;
            
            fat32_set_cluster_value(cluster, next);
            inode->blocks += fat32_fs.sectors_per_cluster;
        }
        cluster = next;
    }

    /* Boucle d'écriture */
    while (count > 0 && cluster != 0 && cluster < FAT32_EOC) {
        uint32_t cluster_pos = current_offset % cluster_size;
        uint32_t to_write = MIN(count, cluster_size - cluster_pos);

        //KDEBUG("Writing %u bytes at cluster %u, pos %u\n", 
        //       to_write, cluster, cluster_pos);

        if (cluster_pos == 0 && count >= cluster_size) {
            uint32_t max_clusters = FAT32_IO_BATCH_SECTORS /
                                    fat32_fs.sectors_per_cluster;
            uint32_t wanted = (uint32_t)(count / cluster_size);
            uint32_t last_cluster;
            uint32_t run;

            if (wanted > max_clusters)
                wanted = max_clusters;
            run = fat32_prepare_contiguous_write_run(cluster, wanted, inode,
                                                     &last_cluster);
            to_write = run * cluster_size;
            if (fat32_write_contiguous_clusters(cluster, run,
                                                buf + bytes_written) != 0) {
                if (cluster_data)
                    kfree(cluster_data);
                return bytes_written ? (ssize_t)bytes_written : -EIO;
            }
            cluster = last_cluster;
        } else {
            if (!cluster_data) {
                cluster_data = kmalloc(cluster_size);
                if (!cluster_data)
                    return bytes_written ? (ssize_t)bytes_written : -ENOMEM;
            }
            if (fat32_read_cluster(cluster, cluster_data) < 0) {
                kfree(cluster_data);
                return bytes_written ? (ssize_t)bytes_written : -EIO;
            }

            memcpy(cluster_data + cluster_pos, buf + bytes_written, to_write);
            if (fat32_write_cluster(cluster, cluster_data) != 0) {
                kfree(cluster_data);
                return bytes_written ? (ssize_t)bytes_written : -EIO;
            }
        }
        
        bytes_written += to_write;
        count -= to_write;
        current_offset += to_write;
        
        /* Passer au cluster suivant si nécessaire */
        if (count > 0) {
            uint32_t next = fat32_get_next_cluster(cluster);
            
            //KDEBUG("Need next cluster: current=%u, next=%u, remaining=%zu\n",
            //       cluster, next, count);
            
            if (next >= FAT32_EOC) {
                /* Allouer un nouveau cluster */
                next = fat32_alloc_cluster();
                if (next == 0) {
                    KERROR("Failed to allocate new cluster\n");
                    break;
                }
                
                //KDEBUG("Allocated new cluster %u, linking from %u\n", next, cluster);
                
                fat32_set_cluster_value(cluster, next);
                inode->blocks += fat32_fs.sectors_per_cluster;
            }
            cluster = next;
        }
    }
    if (cluster_data)
        kfree(cluster_data);
    
    /* Mettre à jour la taille */
    if (current_offset > inode->size) {
        inode->size = current_offset;
        mark_inode_dirty(inode);
        fat32_update_file_size_in_dir(file->name, inode->parent_cluster, inode->size);
    }
    
    file->offset = current_offset;
    inode->mtime = get_current_time();
    
    //KDEBUG("Wrote %zu bytes total\n", bytes_written);
    return bytes_written;
}


ssize_t fat32_file_write2(file_t* file, const void* buffer, size_t count) {
    inode_t* inode = file->inode;
    const char* buf = (const char*)buffer;
    
    if (!buffer || count == 0) return 0;
    if (!inode || !file) return -EINVAL;

    if(!can_write(file)) return -EPERM;

    size_t bytes_written = 0;
    uint32_t current_offset = file->offset;
    uint32_t cluster = inode->first_cluster;
    uint32_t cluster_size = get_fat32_bytes_per_cluster();
    
    KDEBUG("fat32_file_write: current_offset=%u, cluster=%u, buffer=%s, count=%zu\n",
           current_offset, cluster, buf, count);

    if( is_dirty_inodes() ){
        sync_dirty_inodes();
    }

    if( is_fat_dirty() )
    {
        sync_fat_to_disk();
    }

    /* Si le fichier est vide, allouer le premier cluster */
    if (cluster == 0) {
        cluster = fat32_alloc_cluster();
        if (cluster == 0) return -ENOSPC;
        //KDEBUG("fat32_file_write: Allocated cluster=%u\n", cluster);
        
        inode->first_cluster = cluster;
        inode->blocks = fat32_fs.sectors_per_cluster; 
        mark_inode_dirty(inode);

        /* Utiliser le nom exact du fichier */
        const char* filename = file->name;  /* Ex: "test.txt" */
        
        fat32_update_file_by_name(filename, inode->parent_cluster, cluster);

    }
    
    /* Naviguer jusqu'au cluster de départ */
    uint32_t cluster_offset = current_offset / cluster_size;
    //KDEBUG("fat32_file_write: cluster_offset=%u\n", cluster_offset);

    for (uint32_t i = 0; i < cluster_offset && cluster != FAT32_EOC; i++) {
        uint32_t next = fat32_get_next_cluster(cluster);
       //KDEBUG("fat32_file_write: next cluster=%u\n", next);

        if (next >= FAT32_EOC) {
            /* Besoin d'étendre le fichier */
            next = fat32_alloc_cluster();
            if (next == 0) return bytes_written ? (ssize_t)bytes_written : -ENOSPC;
            
            fat32_set_cluster_value(cluster, next);
            inode->blocks += fat32_fs.sectors_per_cluster;
        }
        cluster = next;
    }
    
    while (count > 0 && cluster != 0 && cluster < FAT32_EOC) {
        /* Calculer la position dans le cluster */
        uint32_t cluster_pos = current_offset % cluster_size;
        uint32_t to_write = MIN(count, cluster_size - cluster_pos);

        //KDEBUG("fat32_file_write: cluster_pos=%u, bytes to_write=%u\n", cluster_pos, to_write);


        uint32_t bytes_per_cluster = get_fat32_bytes_per_cluster();
        void* cluster_data = kzalloc(bytes_per_cluster);
        if (!cluster_data) return bytes_written ? (ssize_t)bytes_written : -ENOMEM;
        
        /* Lire le cluster */
        if (fat32_read_cluster(cluster, cluster_data) < 0) {
            KERROR("[fat32_file_write] Failed to read cluster %u\n", cluster);
            kfree(cluster_data);
            return bytes_written ? (ssize_t)bytes_written : -EIO;
        }

        //KDEBUG("fat32_file_write: before memcpy cluster_data=%.64s\n", (char*)cluster_data);

        /* Lire le cluster, le modifier, l'écrire */
        //char* cluster_data = fat32_read_cluster(cluster);
        //if (!cluster_data) break;
        
        memcpy(cluster_data + cluster_pos, buf + bytes_written, to_write);

        //KDEBUG("fat32_file_write: after memcpy cluster_data=%.64s\n", (char*)cluster_data);

        
        if (fat32_write_cluster(cluster, cluster_data) != 0) {
            KERROR("[fat32_file_write] Failed to write cluster %u\n", cluster);
            kfree(cluster_data);
            return bytes_written ? (ssize_t)bytes_written : -EIO;
        }
        
        kfree(cluster_data);
        
        bytes_written += to_write;
        count -= to_write;
        current_offset += to_write;
        
        /* Passer au cluster suivant si nécessaire */
        if (count > 0) {
            uint32_t next = fat32_get_next_cluster(cluster);
            if (next >= FAT32_EOC) {
                /* Allouer un nouveau cluster */
                next = fat32_alloc_cluster();
                if (next == 0) break;
                
                fat32_set_cluster_value(cluster, next);
                inode->blocks += fat32_fs.sectors_per_cluster;
            }
            cluster = next;
        }
    }
    
    /* Mettre à jour la taille du fichier et la position */
    if (current_offset > inode->size) {
        inode->size = current_offset;
        //KDEBUG("Updated inode size to %u\n", inode->size);
        mark_inode_dirty(inode);
        
        /* AJOUTER CECI : Mettre à jour la taille dans l'entrée de répertoire */
        fat32_update_file_size_in_dir( file->name, inode->parent_cluster, inode->size);
        //KDEBUG("Directory entry size update result=%d\n", size_result);
    }
    
    file->offset = current_offset;
    inode->mtime = get_current_time();
    
    KDEBUG("Wrote %zu bytes total\n", bytes_written);

    return bytes_written;
}

static inode_t* fat32_inode_lookup(inode_t* dir, const char* name)
{
    fat32_dir_entry_t* entry;
    inode_t* inode;
    uint32_t first_cluster;
    
    if (!S_ISDIR(dir->mode)) return NULL;
    
    entry = fat32_find_file(dir->first_cluster, name);
    if (!entry) return NULL;
    
    /* Create inode for this file */
    inode = create_inode();
    if (!inode) {
        kfree(entry);
        return NULL;
    }
    
    /* Fill inode from FAT32 entry */
    first_cluster = (entry->first_cluster_hi << 16) | entry->first_cluster_lo;
    
    inode->mode = (entry->attr & FAT_ATTR_DIRECTORY) ? S_IFDIR : S_IFREG;
    inode->mode |= 0777;
    inode->uid = 0;
    inode->gid = 0;
    inode->size = entry->file_size;
    inode->nlink = 1;
    inode->first_cluster = first_cluster;
    inode->parent_cluster = dir->first_cluster;
    
    /* Convert FAT32 dates */
    inode->mtime = fat32_date_to_unix(entry->write_date, entry->write_time);
    inode->atime = inode->mtime;
    inode->ctime = inode->mtime;
    
    /* Assign operations */
    if (S_ISDIR(inode->mode)) {
        inode->i_op = &fat32_inode_ops;
        inode->f_op = &fat32_dir_ops;
    } else {
        inode->i_op = &fat32_inode_ops;
        inode->f_op = &fat32_file_ops;
    }
    
    //KDEBUG("[LOOKUP] '%s' cluster=%u mode=0x%04X f_op=%p\n",
    //       name, inode->first_cluster, inode->mode, inode->f_op);
    kfree(entry);
    return inode;
}

static ssize_t fat32_file_read(file_t* file, void* buffer, size_t count)
{
    inode_t* inode = file->inode;
    uint32_t cluster_size;
    uint32_t cluster;
    uint32_t cluster_skip;
    uint32_t cluster_pos;
    unsigned char* cluster_data;
    unsigned char* output = buffer;
    size_t bytes_read = 0;
    
    if (!buffer || count == 0) return 0;
    if (!file) return -EINVAL;

    if(!can_read(file)) return -EPERM;

    /* Check bounds */
    if (file->offset >= inode->size) {
        return 0; /* EOF */
    }

    if (file->offset + count > inode->size) {
        count = inode->size - file->offset;
    }

    cluster_size = get_fat32_bytes_per_cluster();
    cluster = inode->first_cluster;
    cluster_skip = file->offset / cluster_size;
    cluster_pos = file->offset % cluster_size;

    for (uint32_t i = 0; i < cluster_skip && cluster < FAT32_EOC; i++)
        cluster = fat32_get_next_cluster(cluster);
    if (cluster == 0 || cluster >= FAT32_EOC)
        return 0;

    cluster_data = NULL;

    while (bytes_read < count && cluster != 0 && cluster < FAT32_EOC) {
        size_t available;
        size_t chunk;

        if (cluster_pos == 0 && count - bytes_read >= cluster_size) {
            uint32_t sectors_per_cluster = get_fat32_sectors_per_cluster();
            uint32_t max_clusters = FAT32_IO_BATCH_SECTORS /
                                    sectors_per_cluster;
            uint32_t wanted = (uint32_t)((count - bytes_read) / cluster_size);
            uint32_t last_cluster = cluster;
            uint32_t run = 1;

            if (wanted > max_clusters)
                wanted = max_clusters;
            while (run < wanted) {
                uint32_t next = fat32_get_next_cluster(last_cluster);

                if (next != last_cluster + 1u)
                    break;
                last_cluster = next;
                run++;
            }

            chunk = (size_t)run * cluster_size;
            if (blk_read_sectors(fat32_cluster_to_sector(cluster),
                                 run * sectors_per_cluster,
                                 output + bytes_read) != 0) {
                if (cluster_data)
                    kfree(cluster_data);
                return bytes_read ? (ssize_t)bytes_read : -EIO;
            }
            fat32_note_cluster_reads(run);
            bytes_read += chunk;
            cluster = last_cluster;
            if (bytes_read < count)
                cluster = fat32_get_next_cluster(cluster);
            continue;
        }

        if (!cluster_data) {
            cluster_data = kmalloc(cluster_size);
            if (!cluster_data)
                return bytes_read ? (ssize_t)bytes_read : -ENOMEM;
        }
        if (fat32_read_cluster(cluster, cluster_data) < 0) {
            kfree(cluster_data);
            return bytes_read ? (ssize_t)bytes_read : -EIO;
        }

        available = cluster_size - cluster_pos;
        chunk = MIN(count - bytes_read, available);
        memcpy(output + bytes_read, cluster_data + cluster_pos, chunk);
        bytes_read += chunk;
        cluster_pos = 0;
        if (bytes_read < count)
            cluster = fat32_get_next_cluster(cluster);
    }

    if (cluster_data)
        kfree(cluster_data);
    file->offset += (uint32_t)bytes_read;
    return (ssize_t)bytes_read;
}

static int fat32_file_open(inode_t* inode, file_t* file)
{
    /* Basic validation */
    if (S_ISDIR(inode->mode) && (file->flags & (O_WRONLY | O_RDWR))) {
        return -EISDIR;
    }
    
 /*    if ((file->flags & (O_WRONLY | O_RDWR))) {
        return -EROFS; // Read-only filesystem 
    } */
    
    return 0;
}

static int fat32_file_close(file_t* file)
{
    /* Suppress unused parameter warning */
    (void)file;

    if(is_dirty_inodes())
        sync_dirty_inodes();

    if(is_fat_dirty())
        sync_fat_to_disk();

    /* Nothing special to do for FAT32 */
    return 0;
}

static off_t fat32_file_lseek(file_t* file, off_t offset, int whence)
{
    inode_t* inode = file->inode;
    off_t new_offset;
    
    switch (whence) {
        case SEEK_SET:
            new_offset = offset;
            break;
        case SEEK_CUR:
            new_offset = file->offset + offset;
            break;
        case SEEK_END:
            new_offset = inode->size + offset;
            break;
        default:
            return -EINVAL;
    }
    
    if (new_offset < 0) return -EINVAL;
    
    file->offset = new_offset;
    return new_offset;
}

static int fat32_dir_readdir(file_t* file, dirent_t* dirent)
{
    inode_t* dir_inode = file->inode;
    uint32_t cluster = dir_inode->first_cluster;
    uint32_t target_entry = file->offset;
    uint32_t entry_count = 0;
    char lfn_name[FAT32_MAX_FILENAME + 1];
    uint8_t lfn_checksum = 0;
    bool lfn_pending = false;
    
    if (!S_ISDIR(dir_inode->mode)) {
        KDEBUG("[READDIR] Not a directory (mode: 0x%04X)\n", dir_inode->mode);
        return -ENOTDIR;
    }
    
    //KDEBUG("[READDIR] cluster=%u target=%u inode=%p\n", cluster, target_entry, dir_inode);
    fat32_lfn_clear(lfn_name, sizeof(lfn_name));
    
    /* Parcourir les clusters du repertoire */
    while (cluster && cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t bytes_per_cluster = get_fat32_bytes_per_cluster();
        void* cluster_buf = kmalloc(bytes_per_cluster);
        if (!cluster_buf) {
            KERROR("[READDIR] Failed to allocate cluster buffer\n");
            return -ENOMEM;
        }
        
        /* Lire le cluster */
        if (fat32_read_cluster(cluster, cluster_buf) < 0) {
            KERROR("[READDIR] Failed to read cluster %u\n", cluster);
            kfree(cluster_buf);
            return -EIO;
        }
        
        fat32_dir_entry_t* entries = (fat32_dir_entry_t*)cluster_buf;
        uint32_t entries_per_cluster = bytes_per_cluster / sizeof(fat32_dir_entry_t);
        
        //KDEBUG("[READDIR] Cluster %u: %u entries to check\n", cluster, entries_per_cluster);
        
        /* Parcourir les entrees du cluster */
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t* entry = &entries[i];
            
            /* Fin du repertoire */
            if (entry->name[0] == 0) {
                //KDEBUG("[READDIR] End of directory reached\n");
                kfree(cluster_buf);
                return 0;
            }
            
            /* Ignorer les entrees supprimees */
            if (entry->name[0] == 0xE5) {
                lfn_pending = false;
                fat32_lfn_clear(lfn_name, sizeof(lfn_name));
                continue;
            }

            if (IS_LONG_NAME(entry->attr)) {
                fat32_lfn_entry_t* lfn = (fat32_lfn_entry_t*)entry;

                if (lfn->order & 0x40) {
                    fat32_lfn_clear(lfn_name, sizeof(lfn_name));
                    lfn_checksum = lfn->checksum;
                    lfn_pending = true;
                }

                if (lfn_pending &&
                    lfn->checksum == lfn_checksum &&
                    fat32_lfn_decode_entry(lfn, lfn_name, sizeof(lfn_name)) == 0) {
                    continue;
                }

                lfn_pending = false;
                fat32_lfn_clear(lfn_name, sizeof(lfn_name));
                continue;
            }

            if (entry->attr & FAT_ATTR_VOLUME_ID) {
                lfn_pending = false;
                fat32_lfn_clear(lfn_name, sizeof(lfn_name));
                continue;
            }
            
            /* Si c'est l'entree cible */
            if (entry_count == target_entry) {
                /* Remplir la structure dirent */
                dirent->d_ino = (entry->first_cluster_hi << 16) | entry->first_cluster_lo;
                dirent->d_type = (entry->attr & FAT_ATTR_DIRECTORY) ? DT_DIR : DT_REG;
                
                if (lfn_pending &&
                    fat32_lfn_matches_short(lfn_name, lfn_checksum, entry)) {
                    strncpy(dirent->d_name, lfn_name, sizeof(dirent->d_name) - 1);
                    dirent->d_name[sizeof(dirent->d_name) - 1] = '\0';
                } else {
                    /* Convertir le nom 8.3 */
                    fat32_83_to_name((char*)entry->name, dirent->d_name);
                }
                dirent->d_reclen = sizeof(dirent_t);
                
                /* Incrementer l'offset pour la prochaine lecture */
                file->offset++;
                
                kfree(cluster_buf);
                return 1; /* Succes */
            }
            
            entry_count++;
            lfn_pending = false;
            fat32_lfn_clear(lfn_name, sizeof(lfn_name));
        }
        
        kfree(cluster_buf);
        
        /* Passer au cluster suivant */
        cluster = fat32_get_next_cluster(cluster);
        //KDEBUG("[READDIR] Moving to next cluster: %u\n", cluster);
    }
    
    //KDEBUG("[READDIR] No more entries found\n");
    return 0; /* Fin du repertoire */
}
