#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <fnmatch.h>
#include <limits.h>
#include <libgen.h>

#define BLOCK_SIZE 512
size_t maxSize;
size_t currSize;

typedef struct _block{
    char data[BLOCK_SIZE];
	struct _block* next_block;
}block;

typedef struct _inode{
    int is_dir;// 1 dir 0 regular file
	time_t atime;  
	time_t mtime;  
	time_t ctime; 
	char name[PATH_MAX];
	mode_t mode;
	size_t size;
	int link;
	struct _inode *child;
	struct _inode *next;
	//char* fileData;// NULL if it is not assigned otherwise will have an value
	block* startBlock;
}inode;

inode *root;

inode* searchNode(const char *path){
	if(strcmp(path,"/")==0) {
		return root;
	}
	inode* tmp = root->child;
	while(tmp){
		if(strcmp(tmp->name,path) ==0){
			return tmp;
		}
		inode* newTmp = tmp->child;
		while(newTmp){
			if(strcmp(newTmp->name,path)==0){
				return newTmp;
			}
			newTmp = newTmp->next;
		}
		tmp = tmp->next;
	}
	return NULL;
}

static int ramdisk_getattr(const char *path, struct stat *stbuf)
{
	memset(stbuf, 0, sizeof(struct stat));
	inode* exists = searchNode(path);
	if(exists == NULL) return -ENOENT;
	stbuf->st_size = exists->size;
	stbuf->st_mode = exists->mode;
	stbuf->st_atime = exists->atime;
	stbuf->st_ctime = exists->ctime;
	stbuf->st_mtime = exists->mtime;
	stbuf->st_nlink = exists->link;
	return 0;
}

static int ramdisk_open(const char *path, struct fuse_file_info *fi){
    if(searchNode(path)== NULL) return -ENOENT;
    return 0;
}
static int ramdisk_flush(const char *path, struct fuse_file_info *fi){
	return 0;
}

static int ramdisk_read(const char *path, char *buf, size_t size, off_t offset,struct fuse_file_info *fi){
	int blockSize = BLOCK_SIZE;
	inode* readNode = searchNode(path);
	if(readNode == NULL) return -ENOENT;
	if(readNode->is_dir ==1) return -EISDIR;
	if (size > readNode->size) size = readNode->size;
	block* fileblock = readNode->startBlock;
	
	if (fileblock){
	while (offset - blockSize > 0){
		fileblock = fileblock->next_block;
		offset -= blockSize;
	}
	if (offset > 0) {
		memcpy(buf, fileblock->next_block + offset , blockSize-offset);
		fileblock = fileblock->next_block;
	}
	while(offset < size && fileblock){	
		if( offset + blockSize > size) blockSize = size - offset;
		memcpy(buf + offset, fileblock->data , blockSize);
		fileblock = fileblock->next_block;
		offset += blockSize;
	}			
	return size;
	}else return 0;
}	

static int ramdisk_write(const char *path, const char *buf, size_t size, off_t offset,struct fuse_file_info *fi){
    if((currSize + size) > maxSize) return -ENOSPC;
	inode* nodeWrite = searchNode(path);
	if(nodeWrite == NULL) return -ENOENT;
	if(nodeWrite->is_dir ==1) return -EISDIR;
	block *fileblock, *last;
	int blockSize = BLOCK_SIZE;	
	fileblock = nodeWrite->startBlock;
	if (fileblock == NULL && size > 0) {//creating block if file is new
		offset = 0;
		nodeWrite->size = 0;
		if(currSize + sizeof(block) > maxSize) return -ENOSPC;
		fileblock = (block*)malloc(sizeof(block));
		fileblock->next_block = NULL;
		currSize += sizeof(block);
		nodeWrite->startBlock = fileblock;
	}
	while (offset-blockSize > 0){ //setting the offset
		if(fileblock == NULL) return -1;
		fileblock = fileblock->next_block;
		offset -= blockSize;
	}
	if(offset > 0) {// writting into partial block
		memcpy(fileblock->data + offset, buf, blockSize-offset);
		last = fileblock;
		fileblock = fileblock->next_block;
		offset = blockSize - offset;
	}

	while(offset < size ) {// writing into whole block
		if(offset+blockSize > size) blockSize = size - offset;
		if(currSize + sizeof(block) > maxSize) return -ENOSPC;
		if (fileblock == NULL) {
			fileblock = (block*)malloc(sizeof(block));
			currSize += sizeof(block);
			fileblock->next_block = NULL;
			last->next_block = fileblock;
		}
		memcpy(fileblock->data, buf + offset , blockSize);
		last = fileblock;
		fileblock = fileblock->next_block;
		offset += blockSize;
	}
	nodeWrite->size += size;
	nodeWrite->atime = time(NULL);
	nodeWrite->mtime = time(NULL);
	return size;	 
}

static int ramdisk_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
	inode *exists = searchNode(path);
	if(exists){
		inode *iter;
		filler(buf, ".", NULL, 0);
		filler(buf, "..", NULL, 0);
		iter = exists->child;
		while(iter != NULL){
			filler(buf,strrchr(iter->name, '/')+1, NULL, 0);
			iter = iter->next;
		}
        return 0;                
	}
	else
		return -ENOENT;
}

void insertNode(inode *tmpNode, inode *tmpDir)
{
	if(tmpNode->child) {
		inode *iter;
                for(iter = tmpNode->child; iter->next != NULL; iter = iter->next)
                	;
                iter->next = tmpDir;			
	}
	else
		tmpNode->child = tmpDir;
}

static int ramdisk_create(const char *path, mode_t mode, struct fuse_file_info *fi) 
{
	if (searchNode(path) == NULL){
		if ((currSize + sizeof(inode)) < maxSize){
		inode *file = (inode *) malloc(sizeof(inode));
		currSize += sizeof(inode);
		strcpy(file->name, path);
		file->is_dir = 0;
		file->mode = mode;
		file->atime = time(NULL);
		file->mtime = time(NULL);
		file->ctime = time(NULL);
		file->size = 0;
		file->link = 0;
		file->next = NULL;
		file->child = NULL;
		file->startBlock = NULL;
		char tmpPath[PATH_MAX];
		strcpy(tmpPath, path);
		char* dirName = dirname(tmpPath); 
		inode *dirNode = searchNode(dirName);
		insertNode(dirNode,file);
		dirNode->link +=1;
		return 0;
		}else{
			return -ENOSPC;
		}
	}
	return -EEXIST;
}

static int ramdisk_mkdir(const char *path, mode_t mode)
{
	if (searchNode(path) == NULL){
		if ((currSize + sizeof(inode)) < maxSize){
		inode *dir = (inode *) malloc(sizeof(inode));
		currSize += sizeof(inode);
		strcpy(dir->name, path);
		dir->is_dir = 1;
		dir->mode = S_IFDIR | 0755;
		dir->atime = time(NULL);
		dir->mtime = time(NULL);
		dir->ctime = time(NULL);
		dir->next = NULL;
		dir->link = 2;
		dir->size = 0;
		dir->child = NULL;
		dir->startBlock = NULL;
		char tmpPath[PATH_MAX];
		strcpy(tmpPath, path);
		char* dirName = dirname(tmpPath); 
		inode *temp = searchNode(dirName);
		insertNode(temp,dir);
		return 0;
		}else return -ENOSPC;
	}
	return -EEXIST;
}

void deleteNode(inode *dir, inode *node){
	if(dir->child == node){
		if(dir->child->next != NULL) dir->child = node->next;
		else dir->child = NULL;
	}else{
		inode *temp = dir->child;
		while(temp!= NULL){
			if(temp->next == node){
				temp->next = temp->next->next;
				break;
			}
		temp = temp->next;
		}
	}
}

static int ramdisk_unlink(const char *path){
	inode* nodeTodelete = searchNode(path);
	if(nodeTodelete == NULL) return -ENOENT;
	if(nodeTodelete->size > 0){
		ramdisk_truncate(path,0);
	}
	char tmpPath[PATH_MAX];
	strcpy(tmpPath, path);
	char* dirName = dirname(tmpPath); 
	inode *dirNode = searchNode(dirName);
	deleteNode(dirNode, nodeTodelete);
	free(nodeTodelete);
	currSize -= sizeof(inode);
	return 0;
}

static int ramdisk_rmdir(const char *path){
	inode* nodeTodelete = searchNode(path);
	if(nodeTodelete== NULL) return -ENOENT;
	if(nodeTodelete->child!= NULL) return -ENOTEMPTY;
	char tmpPath[PATH_MAX];
	strcpy(tmpPath, path);
	char* dirName = dirname(tmpPath); 
	inode *dirNode = searchNode(dirName);
	deleteNode(dirNode, nodeTodelete);
	free(nodeTodelete);
	dirNode->link -=1;
	currSize -= sizeof(inode);
	return 0;
}


static int ramdisk_opendir(const char *path, struct fuse_file_info *fu)
{
	if(searchNode(path) == NULL)	
		return -ENOENT;
	else
		return 0;
}

int ramdisk_truncate(const char *path, off_t offset)
{ 
	int blocksize = BLOCK_SIZE;
	inode* tNode = searchNode(path);
	if(tNode == NULL) return -ENOENT;
	if(tNode->is_dir ==1) return -EISDIR;
	if ( offset >= tNode->size )return 0;
	block* fileblock = tNode->startBlock;
	if(fileblock == NULL)return 0;
	tNode->size = offset;
    
	while (offset- blocksize > 0 ) {
		offset -= blocksize;
		fileblock = fileblock->next_block;
	}

	if ( offset > 0 ) {
		memset(fileblock->data + offset, '\0', blocksize-offset);
		offset = blocksize - offset;
		fileblock = fileblock->next_block;
	}
	while (fileblock) {
		block* blockTofree = fileblock;
		fileblock = fileblock->next_block;
		currSize-= sizeof(block);
		free(blockTofree);
	}
	if (offset == 0)tNode->startBlock = NULL;	
	return 0;	
}

inode* getNode(inode *dir, inode *node){
	inode* result;
	if(dir->child == node){
		result = dir->child;
		if(dir->child->next != NULL) dir->child = node->next;
		else dir->child = NULL;
	}else{
		inode *temp = dir->child;
		while(temp!= NULL){
			if(temp->next == node){
				result = temp->next;
				temp->next = temp->next->next;
				break;
			}
		temp = temp->next;
		}
	}
	return result;
}

int ramdisk_rename(const char *from , const char *to){// renaming only file
	inode* nodeFrom = searchNode(from);
	if(nodeFrom == NULL) return -ENOENT;
	inode* nodeTo = searchNode(to);
	if(nodeFrom->is_dir == 0){
		if(nodeTo != NULL && nodeTo->is_dir ==1) return -EISDIR;
		if(nodeTo != NULL && nodeTo->is_dir ==0)ramdisk_unlink(to);
		char tmpPath[PATH_MAX];
		strcpy(tmpPath, from);
		char *dirFrom = dirname(tmpPath); 
		inode* dirNodeFrom = searchNode(dirFrom);
		inode* result = getNode(dirNodeFrom,nodeFrom);
		dirNodeFrom->link -=1;
		strcpy(result->name,to);
		strcpy(tmpPath, to);
		char *dirTo = dirname(tmpPath); 
		inode *dirNodeTo = searchNode(dirTo);
		insertNode(dirNodeTo,result);
	}
	return 0;
}

struct fuse_operations ramdisk_oper = {
	.getattr        = ramdisk_getattr,
    .open           = ramdisk_open,
    .flush          = ramdisk_flush,
    .read           = ramdisk_read, 
    .write          = ramdisk_write,
    .create         = ramdisk_create,
    .mkdir          = ramdisk_mkdir,
    .unlink         = ramdisk_unlink,
    .rmdir          = ramdisk_rmdir,
    .opendir        = ramdisk_opendir,
    .readdir        = ramdisk_readdir,
	.truncate 		= ramdisk_truncate,
	.rename			= ramdisk_rename
};

void ramdisk_init() 
{
	root = (inode *)malloc(sizeof(inode));
	currSize += sizeof(inode);
	strcpy(root->name, "/");
	root->is_dir = 1;
	root->ctime = time(NULL);
	root->atime = time(NULL);
	root->mtime = time(NULL);
	root->child = NULL;
	root->next = NULL;
	root->startBlock = NULL;
	root->mode = S_IFDIR | 755;
	root->link = 2;
}

int main(int argc, char *argv[])
{
	// maxSize = atoi(argv[3])*1024*1024;
	// ramdisk_init();

	// argc = 3;
	// return fuse_main(argc, argv, &ramdisk_oper, NULL);
	if (argc < 3) {
		fprintf(stderr, "Usage: %s <mount-directory> <sizeinMB>\n", argv[0]);
		return -1;
	}
	if(argc > 3){
		fprintf(stderr, "To many arguments.\nUsage: %s <mount-directory> <sizeinMB>\n", argv[0]);
		return -1;
	}

	maxSize = atoi(argv[2])*1024*1024;
	ramdisk_init();

	argc = 2;
	return fuse_main(argc, argv, &ramdisk_oper, NULL);
}
