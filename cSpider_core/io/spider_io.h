/*************************************************************************
	> File Name: spider_io.h
	> Author: DingJing
	> Mail: dingjing_ding@staff.easou.com
	> Created Time: 2017年08月10日 星期四 16时54分21秒
 ************************************************************************/
#ifndef _SPIDER_IO_H
#define _SPIDER_IO_H
#include <iostream>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>


namespace CSpider {
namespace IO {

    /*  类开始  */
    class SpiderIO {
    public:
        SpiderIO(std::string dir, std::string file, std::string content):dir(dir),file(file),content(content){}
        ~SpiderIO(){}

        void io_write_run(); 

    protected:
        bool dir_is_exist();
        void dir_create();
        void file_write(); 

    private:
        std::string       dir;                //  存放文件夹
        std::string       file;               //  文件名
        std::string       content;            //  内容
    
    }; // 类结束
}}
#endif