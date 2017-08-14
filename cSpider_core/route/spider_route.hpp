/*************************************************************************
	> File Name: spider_route.hpp
	> Author: DingJing
	> Mail: dingjing_ding@staff.easou.com
	> Created Time: 2017年08月11日 星期五 14时31分11秒
 ************************************************************************/

#include <iostream>
#include <list>
#include <string>
#include <cstdlib>
#include <cstring>

namespace CSpider {
namespace Route {
    class SpiderRoute {

    public:
        SpiderRoute(std::string& html):html(html) {}
        ~SpiderRoute(){}
        
        void route_run() {

            get_url();
            output_url();
        }

    protected:
        void get_url() {

            int                 startNum = 0;
            int                 endNum = 0;
            int                 pos = 0;
            std::string         oo;
            std::string         buf;
            std::string         url;
            std::string         href = "<a href=";

            // 开始解析链接
            while ((endNum = html.find_first_of(href, startNum)) != std::string::npos) {

                pos = endNum;                                                   // 记录此时的位置
                startNum = endNum + 4;                                          // 移到内容后边
                endNum = html.find_first_of("\"", startNum);                    // 找到结束的位置
                if(endNum != std::string::npos) {
                    url = html.substr(pos, endNum - pos);
                    std::cout << url << std::endl;

                    getchar();
                }

                //std::cin > oo;
                startNum = endNum + 1;
            }






















/*
            // 同时查找 href base href
            for(int i = 0; i < htmlLen - 14; ++i) {

                // 找到一天链接
                if(0 == strncmp(pHtml + i, "href=", 5) || 0 == strncmp(pHtml + i, "href =", 6)) {

                    std::cout << "进来了" << std::endl;

                    // 找到引号
                    int             len = 0;
                    int             j = 0;
                    int             k = 0;

                    // 开始解析 ---
                    for (j = ++i; *(pHtml + j) != '\"' && *(pHtml + j) != '\''; ++j);
                    std::cout << *(pHtml + j) ;

                    getchar();

                    break;
                }
                    //const char* pHead = pHtml + j;
                    //for (k = ++j; *(pHtml + j) != '\"' && *(pHtml + k) != '\''; ++k);
                    //const char* pEnd = pHtml + k;

                    // 去头尾
                    //if(*pHead == '\'' || *pHead == '\"')
                    //    ++j;
                    //if(*pEnd == '\'' || *pEnd == '\"')
                    //    --k;

                    //len = k - j;

                    // 保存链接
                    //memset(buf, 0, sizeof(buf));
                    //strncmp(buf, pHead, len);
                    //url = buf;

                    //std::cout << buf << std::endl;
                    //urlList.push_back(url);
                    //i += len;
                //}
*/
                /*
                // 找到一条 base 链接
                if(0 == strncmp(pHtml + i, "base href=", 10) || 0 == strncmp(pHtml + i, "base href =", 11)) {

                     // 找到引号
                    int             len = 0;
                    int             j = 0;
                    int             k = 0;

                    // 开始解析 ---
                    for(j = ++i; *(pHtml + j) != '\"' && *(pHtml + j) != '\''; ++j);
                    const char* pHead = pHtml + j;
                    for(k = ++j; *(pHtml + j) != '\"' && *(pHtml + k) != '\''; ++k);
                    const char* pEnd = pHtml + k;

                    // 去头尾
                    if(*pHead == '\'' || *pHead == '\"')
                        ++j;
                    if(*pEnd == '\'' || *pEnd == '\"')
                        --k;

                    len = k - j;

                    // 保存链接
                    memset(buf, 0, sizeof(buf));
                    strncmp(buf, pHead, len);
                    url = buf;

                    baseUrlList.push_back(url);
                    i += len;
                }
            } 
             */   

        } // get_url 结束

        // url 简单处理
        void dispose_url() {

            
        } // url 处理结束

        void output_url() {

            std::cout << "ok" << std::endl;

            for(std::list<std::string>::iterator it = urlList.begin(); it != urlList.end(); ++ it) {

                std::cout << "url: " << *it << std::endl;
            }
            
        }


    private:
        std::string             html;
        std::list<std::string>  urlList;
        std::list<std::string>  baseUrlList;


    }; // 类结束
}
}




























































