#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <QObject>
#include <QString>
#include <QJsonDocument>

// hyperion / utils
#include <utils/Logger.h>

// settings
#include <utils/settings.h>

class BonjourServiceRegister;
class StaticFileServing;
class QtHttpServer;

class WebServer : public QObject {
	Q_OBJECT

public:
	WebServer (const QJsonDocument& config, QObject * parent = 0);

	virtual ~WebServer (void);

	void start();
	void stop();

	const bool isListening();
	quint16 getPort() { return _port; };

	///
	/// @brief Set a new description, if empty the description is NotFound for clients
	///
	void setSSDPDescription(const QString & desc);

signals:
	///
	/// @emits whenever server is started or stopped (to sync with SSDPHandler)
	/// @param newState   True when started, false when stopped
	///
	void stateChange(const bool newState);

public slots:
	void onServerStopped      (void);
	void onServerStarted      (quint16 port);
	void onServerError        (QString msg);

	///
	/// @brief Handle settings update from Hyperion Settingsmanager emit or this constructor
	/// @param type   settingyType from enum
	/// @param config configuration object
	///
	void handleSettingsUpdate(const settings::type& type, const QJsonDocument& config);

private:
	Logger*              _log;
	QString              _baseUrl;
	quint16              _port;
	StaticFileServing*   _staticFileServing;
	QtHttpServer*        _server;

	const QString        WEBSERVER_DEFAULT_PATH = ":/webconfig";
	const quint16        WEBSERVER_DEFAULT_PORT = 8090;

	BonjourServiceRegister * _serviceRegister = nullptr;
};

#endif // WEBSERVER_H
