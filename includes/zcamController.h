class ZCAMController {
private:
	bool stop = false;
	string server;
    int start_hour = 6;   // 6 AM
    int end_hour = 22;    // 10 PM

    string camera_ip;
    string camera_id;
    string rtsp_url;
    string http_base_url;

public:
	ZCAMController(json config);
};