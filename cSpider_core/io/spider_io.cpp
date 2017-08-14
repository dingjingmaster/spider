/*************************************************************************
	> File Name: spider_io.cpp
	> Author: DingJing
	> Mail: dingjing_ding@staff.easou.com
	> Created Time: 2017年08月14日 星期一 17时39分31秒
 ************************************************************************/

#include <spider_io.h>

using namespace CSpider::IO;

void SpiderIO::io_write_run() {
    // 开始写
    if(!dir_is_exist()) {

        dir_create();
    }

    file_write();

    return;
}

bool SpiderIO::dir_is_exist() {

    if (dir.empty()) {

        return false;
    }

    int           ret;
    struct stat   dirInfo;

    if(opendir(dir.c_str()) == NULL) {

                ////////
        return false;
    }

    return true;
}

void SpiderIO::dir_create() {

    if(dir.empty()) {

        return;
    }

    if(!mkdir(dir.c_str(), 0770)) {

        return; // 失败
    }
}

void SpiderIO::file_write() {

    if(file.empty()) {

        return;
    }

    int     fd;
    int     ret;

    // 切换目录
    if(-1 == chdir(dir.c_str())) {
                
                ////// 不存在
        return;
    }

    // 打开文件并执行写入操作
    fd = open(file.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0770);
    if(-1 == fd) {
                
        return; //error
    }

    ret = write(fd, content.c_str(), content.length());
    if(ret < content.length()) {
                
        return; // error
    }

    close(fd);

    return;
}


