#ifndef __HTTP_UPDATE_SERVER_H
#define __HTTP_UPDATE_SERVER_H

class RepRapWebServer;

class MksHTTPUpdateServer{
  private:
    bool _serial_output;
    RepRapWebServer *_server;
    static const char *_serverIndex;
    static const char *_failedResponse;
    static const char *_successResponse;
    char * _username;
    char * _password;
    bool _authenticated;
  public:
    MksHTTPUpdateServer(bool serial_debug=false);

    void setup(RepRapWebServer *server)  {
      setup(server, NULL, NULL);
    }
    void setup(RepRapWebServer *server,  const char * username, const char * password);
};

#endif
