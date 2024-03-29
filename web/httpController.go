package web

import (
	"encoding/json"
	"io/ioutil"
	"net/http"
	"spider/runtime/user"
	"text/template"

	"spider/common/session"
	"spider/config"
	"spider/logs"
)

type UserReqInfo struct {
	name 			string 		`name`
	password		string		`password`
}

var globalSessions *session.Manager

func init() {
	globalSessions, _ = session.NewManager("memory",
		`{
					"cookieName":"spiderSession",
					"enableSetCookie,omitempty": true,
					"secure": false,
					"sessionIDHashFunc": "sha1",
					"sessionIDHashKey": "",
					"cookieLifeTime": 157680000,
					"providerConfig": ""
				}`)
	// go globalSessions.GC()
}

// 处理web页面请求
func IndexHandle (rw http.ResponseWriter, req *http.Request) {
	sess, _ := globalSessions.SessionStart(rw, req)
	defer sess.SessionRelease(rw)
	//index, _ := views_index_html()
	//t, err := template.New("index").Parse(string(index)) 	//解析模板文件
	t, err := template.ParseFiles("web/views/index.html") 	//解析模板文件
	if err != nil {
		logs.Log.Error("%v", err)
	}

	data := map[string]interface{}{
		"title":   config.FullName,
		"logo":    config.IconPng,
	}
	t.Execute(rw, data) //执行模板的merger操作
}

func LoginHandle (rw http.ResponseWriter, req *http.Request) {
	body, _ := ioutil.ReadAll(req.Body)
	var muser UserReqInfo
	err := json.Unmarshal(body, &muser)
	if nil != err {
		logs.Log.Warning("user:%s login error:%v", muser.name, err)
		return
	}

	if user.GAllUsers.ValidUser(muser.name) {
		user.GAllUsers.NewUser(muser.name)
		rw.Write([]byte("login"))
		logs.Log.Informational("user:%s login success", muser.name)
	}
}

func SpiderHandle (rw http.ResponseWriter, req *http.Request) {
	t, err := template.ParseFiles("web/views/spider.html") 	//解析模板文件
	if err != nil {
		logs.Log.Error("%v", err)
	}

	data := map[string]interface{}{
		"title":   config.FullName,
		"logo":    config.IconPng,
	}
	t.Execute(rw, data) //执行模板的merger操作
}

