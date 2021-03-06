// project includes
#include <api/JsonAPI.h>

// stl includes
#include <iostream>
#include <iterator>

// Qt includes
#include <QResource>
#include <QDateTime>
#include <QCryptographicHash>
#include <QImage>
#include <QBuffer>
#include <QByteArray>
#include <QFileInfo>
#include <QDir>
#include <QIODevice>
#include <QDateTime>

// hyperion includes
#include <utils/jsonschema/QJsonFactory.h>
#include <utils/SysInfo.h>
#include <HyperionConfig.h>
#include <utils/ColorSys.h>
#include <leddevice/LedDevice.h>
#include <hyperion/GrabberWrapper.h>
#include <utils/Process.h>
#include <utils/JsonUtils.h>
#include <utils/Stats.h>

// bonjour wrapper
#include <bonjour/bonjourbrowserwrapper.h>

// ledmapping int <> string transform methods
#include <hyperion/ImageProcessor.h>

// api includes
#include <api/JsonCB.h>

// plugin
#include <plugin/Plugins.h>

// auth manager
#include <hyperion/AuthManager.h>

using namespace hyperion;

JsonAPI::JsonAPI(QString peerAddress, Logger* log, QObject* parent, bool noListener)
	: QObject(parent)
	, _authManager(AuthManager::getInstance())
	, _authorized(false)
	, _userAuthorized(false)
	, _apiAuthRequired(_authManager->isAuthRequired())
	, _jsonCB(new JsonCB(this))
	, _noListener(noListener)
	, _peerAddress(peerAddress)
	, _log(log)
	, _hyperion(Hyperion::getInstance())
	, _streaming_logging_activated(false)
	, _image_stream_timeout(0)
{
	// setup auth interface, is GLOBAL
	connect(_authManager, &AuthManager::newPendingTokenRequest, this, &JsonAPI::handlePendingTokenRequest);
	connect(_authManager, &AuthManager::tokenResponse, this, &JsonAPI::handleTokenResponse);

	// the JsonCB creates json messages you can subscribe to e.g. data change events; forward them to the parent client
	connect(_jsonCB, &JsonCB::newCallback, this, &JsonAPI::callbackMessage);
	// notify hyperion about a jsonMessageForward
	connect(this, &JsonAPI::forwardJsonMessage, _hyperion, &Hyperion::forwardJsonMessage);

	// connect to plugin
	_plugins = _hyperion->getPluginsInstance();
	// from Plugins
	connect(_plugins, &Plugins::pluginAction, this, &JsonAPI::doPluginAction);
	// to Plugins
	connect(this, &JsonAPI::pluginAction, _plugins, &Plugins::doPluginAction);

	// led color stream update timer
	connect(&_timer_ledcolors, SIGNAL(timeout()), this, SLOT(streamLedcolorsUpdate()));
	_image_stream_mutex.unlock();
}

void JsonAPI::handleMessage(const QString& messageString, const QString& httpAuthHeader)
{
	const QString ident = "JsonRpc@"+_peerAddress;
	Q_INIT_RESOURCE(JSONRPC_schemas);
	QJsonObject message;
	// parse the message
	if(!JsonUtils::parse(ident, messageString, message, _log))
	{
		sendErrorReply("Errors during message parsing, please consult the Hyperion Log.");
		return;
	}

	// check basic message
	if(!JsonUtils::validate(ident, message, ":schema", _log))
	{
		sendErrorReply("Errors during message validation, please consult the Hyperion Log.");
		return;
	}

	// check specific message
	const QString command = message["command"].toString();
	if(!JsonUtils::validate(ident, message, QString(":schema-%1").arg(command), _log))
	{
		sendErrorReply("Errors during specific message validation, please consult the Hyperion Log");
		return;
	}

	// client auth before everything else but not for http
	int tan = message["tan"].toInt();
	if (!_noListener && command == "authorize")
	{
		handleAuthorizeCommand(message, command, tan);
		return;
	}

	// on the fly auth available for http from http Auth header, on failure we return and auth handler sends a failure
	if(_noListener && _apiAuthRequired && !_authorized)
	{
		// extract token from http header
		QString cToken = httpAuthHeader.mid(5).trimmed();
		if(!handleHTTPAuth(command, tan, cToken))
			return;
	}

	// on strong api auth you need a auth for all cmds
	if(_apiAuthRequired && !_authorized)
	{
		sendErrorReply("No Authorization", command, tan);
		return;
	}

	// switch over all possible commands and handle them
	if      (command == "color")          handleColorCommand         (message, command, tan);
	else if (command == "image")          handleImageCommand         (message, command, tan);
	else if (command == "effect")         handleEffectCommand        (message, command, tan);
	else if (command == "create-effect")  handleCreateEffectCommand  (message, command, tan);
	else if (command == "delete-effect")  handleDeleteEffectCommand  (message, command, tan);
	else if (command == "sysinfo")        handleSysInfoCommand       (message, command, tan);
	else if (command == "serverinfo")     handleServerInfoCommand    (message, command, tan);
	else if (command == "clear")          handleClearCommand         (message, command, tan);
	else if (command == "adjustment")     handleAdjustmentCommand    (message, command, tan);
	else if (command == "sourceselect")   handleSourceSelectCommand  (message, command, tan);
	else if (command == "config")         handleConfigCommand        (message, command, tan);
	else if (command == "componentstate") handleComponentStateCommand(message, command, tan);
	else if (command == "ledcolors")      handleLedColorsCommand     (message, command, tan);
	else if (command == "logging")        handleLoggingCommand       (message, command, tan);
	else if (command == "processing")     handleProcessingCommand    (message, command, tan);
	else if (command == "videomode")      handleVideoModeCommand     (message, command, tan);
	else if (command == "plugin")         handlePluginCommand        (message, command, tan);
	else                                  handleNotImplemented       ();
}

void JsonAPI::handleColorCommand(const QJsonObject& message, const QString& command, const int tan)
{
	emit forwardJsonMessage(message);

	// extract parameters
	int priority = message["priority"].toInt();
	int duration = message["duration"].toInt(-1);
	QString origin = message["origin"].toString() + "@"+_peerAddress;

	std::vector<ColorRgb> colorData(_hyperion->getLedCount());
	const QJsonArray & jsonColor = message["color"].toArray();
	unsigned int i = 0;
	for (; i < unsigned(jsonColor.size()/3) && i < _hyperion->getLedCount(); ++i)
	{
		colorData[i].red = uint8_t(jsonColor.at(3u*i).toInt());
		colorData[i].green = uint8_t(jsonColor.at(3u*i+1u).toInt());
		colorData[i].blue = uint8_t(jsonColor.at(3u*i+2u).toInt());
	}

	// copy full blocks of led colors
	unsigned size = i;
	while (i + size < _hyperion->getLedCount())
	{
		memcpy(&(colorData[i]), colorData.data(), size * sizeof(ColorRgb));
		i += size;
	}

	// copy remaining block of led colors
	if (i < _hyperion->getLedCount())
	{
		memcpy(&(colorData[i]), colorData.data(), (_hyperion->getLedCount()-i) * sizeof(ColorRgb));
	}

	// register and set color
	_hyperion->registerInput(priority, hyperion::COMP_COLOR, origin);
	_hyperion->setInput(priority, colorData, duration);

	// send reply
	sendSuccessReply(command, tan);
}

void JsonAPI::handleImageCommand(const QJsonObject& message, const QString& command, const int tan)
{
	emit forwardJsonMessage(message);

	// extract parameters
	int priority = message["priority"].toInt();
	int duration = message["duration"].toInt(-1);
	int width = message["imagewidth"].toInt();
	int height = message["imageheight"].toInt();
	QByteArray data = QByteArray::fromBase64(QByteArray(message["imagedata"].toString().toUtf8()));

	// check consistency of the size of the received data
	if (data.size() != width*height*3)
	{
		sendErrorReply("Size of image data does not match with the width and height", command, tan);
		return;
	}

	// create ImageRgb
	Image<ColorRgb> image(width, height);
	memcpy(image.memptr(), data.data(), data.size());

	_hyperion->registerInput(priority, hyperion::COMP_IMAGE, "JsonRpc@"+_peerAddress);
	_hyperion->setInputImage(priority, image, duration);

	// send reply
	sendSuccessReply(command, tan);
}

void JsonAPI::handleEffectCommand(const QJsonObject& message, const QString& command, const int tan)
{
	emit forwardJsonMessage(message);

	// extract parameters
	int priority = message["priority"].toInt();
	int duration = message["duration"].toInt(-1);
	QString pythonScript = message["pythonScript"].toString();
	QString origin = message["origin"].toString() + "@"+_peerAddress;
	const QJsonObject & effect = message["effect"].toObject();
	const QString & effectName = effect["name"].toString();

	// set output
	if (effect.contains("args"))
	{
		_hyperion->setEffect(effectName, effect["args"].toObject(), priority, duration, pythonScript, origin);
	}
	else
	{
		_hyperion->setEffect(effectName, priority, duration, origin);
	}

	// send reply
	sendSuccessReply(command, tan);
}

void JsonAPI::handleCreateEffectCommand(const QJsonObject& message, const QString &command, const int tan)
{
	if (!message["args"].toObject().isEmpty())
	{
		QString scriptName;
		(message["script"].toString().mid(0, 1)  == ":" )
			? scriptName = ":/effects//" + message["script"].toString().mid(1)
			: scriptName = message["script"].toString();

		std::list<EffectSchema> effectsSchemas = _hyperion->getEffectSchemas();
		std::list<EffectSchema>::iterator it = std::find_if(effectsSchemas.begin(), effectsSchemas.end(), find_schema(scriptName));

		if (it != effectsSchemas.end())
		{
			if(!JsonUtils::validate("JsonRpc@"+_peerAddress, message["args"].toObject(), it->schemaFile, _log))
			{
				sendErrorReply("Error during arg validation against schema, please consult the Hyperion Log", command, tan);
				return;
			}

			QJsonObject effectJson;
			QJsonArray effectArray;
			effectArray = _hyperion->getQJsonConfig()["effects"].toObject()["paths"].toArray();

			if (effectArray.size() > 0)
			{
				if (message["name"].toString().trimmed().isEmpty() || message["name"].toString().trimmed().startsWith("."))
				{
					sendErrorReply("Can't save new effect. Effect name is empty or begins with a dot.", command, tan);
					return;
				}

				effectJson["name"] = message["name"].toString();
				effectJson["script"] = message["script"].toString();
				effectJson["args"] = message["args"].toObject();

				std::list<EffectDefinition> availableEffects = _hyperion->getEffects();
				std::list<EffectDefinition>::iterator iter = std::find_if(availableEffects.begin(), availableEffects.end(), find_effect(message["name"].toString()));

				QFileInfo newFileName;
				if (iter != availableEffects.end())
				{
					newFileName.setFile(iter->file);
					if (newFileName.absoluteFilePath().mid(0, 1)  == ":")
					{
						sendErrorReply("The effect name '" + message["name"].toString() + "' is assigned to an internal effect. Please rename your effekt.", command, tan);
						return;
					}
				} else
				{
					QString f = FileUtils::convertPath(effectArray[0].toString() + "/" + message["name"].toString().replace(QString(" "), QString("")) + QString(".json"));
					newFileName.setFile(f);
				}

				if(!JsonUtils::write(newFileName.absoluteFilePath(), effectJson, _log))
				{
					sendErrorReply("Error while saving effect, please check the Hyperion Log", command, tan);
					return;
				}

				Info(_log, "Reload effect list");
				_hyperion->reloadEffects();
				sendSuccessReply(command, tan);
			} else
			{
				sendErrorReply("Can't save new effect. Effect path empty", command, tan);
				return;
			}
		} else
			sendErrorReply("Missing schema file for Python script " + message["script"].toString(), command, tan);
	} else
		sendErrorReply("Missing or empty Object 'args'", command, tan);
}

void JsonAPI::handleDeleteEffectCommand(const QJsonObject& message, const QString& command, const int tan)
{
	QString effectName = message["name"].toString();
	std::list<EffectDefinition> effectsDefinition = _hyperion->getEffects();
	std::list<EffectDefinition>::iterator it = std::find_if(effectsDefinition.begin(), effectsDefinition.end(), find_effect(effectName));

	if (it != effectsDefinition.end())
	{
		QFileInfo effectConfigurationFile(it->file);
		if (effectConfigurationFile.absoluteFilePath().mid(0, 1)  != ":" )
		{
			if (effectConfigurationFile.exists())
			{
				bool result = QFile::remove(effectConfigurationFile.absoluteFilePath());
				if (result)
				{
					Info(_log, "Reload effect list");
					_hyperion->reloadEffects();
					sendSuccessReply(command, tan);
				} else
					sendErrorReply("Can't delete effect configuration file: " + effectConfigurationFile.absoluteFilePath() + ". Please check permissions", command, tan);
			} else
				sendErrorReply("Can't find effect configuration file: " + effectConfigurationFile.absoluteFilePath(), command, tan);
		} else
			sendErrorReply("Can't delete internal effect: " + message["name"].toString(), command, tan);
	} else
		sendErrorReply("Effect " + message["name"].toString() + " not found", command, tan);
}

void JsonAPI::handleSysInfoCommand(const QJsonObject&, const QString& command, const int tan)
{
	// create result
	QJsonObject result;
	QJsonObject info;
	result["success"] = true;
	result["command"] = command;
	result["tan"] = tan;

	SysInfo::HyperionSysInfo data = SysInfo::get();
	QJsonObject system;
	system["kernelType"    ] = data.kernelType;
	system["kernelVersion" ] = data.kernelVersion;
	system["architecture"  ] = data.architecture;
	system["wordSize"      ] = data.wordSize;
	system["productType"   ] = data.productType;
	system["productVersion"] = data.productVersion;
	system["prettyName"    ] = data.prettyName;
	system["hostName"      ] = data.hostName;
	system["domainName"    ] = data.domainName;
	info["system"] = system;

	QJsonObject hyperion;
	hyperion["jsonrpc_version" ] = QString(HYPERION_JSON_VERSION);
	hyperion["version"         ] = QString(HYPERION_VERSION);
	hyperion["build"           ] = QString(HYPERION_BUILD_ID);
	hyperion["time"            ] = QString(__DATE__ " " __TIME__);
	hyperion["id"              ] = Stats::getInstance()->getID();
	info["hyperion"] = hyperion;

	// send the result
	result["info" ] = info;
	emit callbackMessage(result);
}

void JsonAPI::handleServerInfoCommand(const QJsonObject& message, const QString& command, const int tan)
{
	QJsonObject info;

	// collect priority information
	QJsonArray priorities;
	uint64_t now = QDateTime::currentMSecsSinceEpoch();
	QList<int> activePriorities = _hyperion->getActivePriorities();
	activePriorities.removeAll(255);
	int currentPriority = _hyperion->getCurrentPriority();

	foreach (int priority, activePriorities) {
		const Hyperion::InputInfo & priorityInfo = _hyperion->getPriorityInfo(priority);
		QJsonObject item;
		item["priority"] = priority;
		if (int(priorityInfo.timeoutTime_ms - now) > -1 )
		{
			item["duration_ms"] = int(priorityInfo.timeoutTime_ms - now);
		}
		// owner has optional informations to the component
		if(!priorityInfo.owner.isEmpty())
			item["owner"] = priorityInfo.owner;

		item["componentId"] = QString(hyperion::componentToIdString(priorityInfo.componentId));
		item["origin"] = priorityInfo.origin;
		item["active"] = (priorityInfo.timeoutTime_ms >= -1);
		item["visible"] = (priority == currentPriority);

		if(priorityInfo.componentId == hyperion::COMP_COLOR && !priorityInfo.ledColors.empty())
		{
			QJsonObject LEDcolor;

			// add RGB Value to Array
			QJsonArray RGBValue;
			RGBValue.append(priorityInfo.ledColors.begin()->red);
			RGBValue.append(priorityInfo.ledColors.begin()->green);
			RGBValue.append(priorityInfo.ledColors.begin()->blue);
			LEDcolor.insert("RGB", RGBValue);

			uint16_t Hue;
			float Saturation, Luminace;

			// add HSL Value to Array
			QJsonArray HSLValue;
			ColorSys::rgb2hsl(priorityInfo.ledColors.begin()->red,
					priorityInfo.ledColors.begin()->green,
					priorityInfo.ledColors.begin()->blue,
					Hue, Saturation, Luminace);

			HSLValue.append(Hue);
			HSLValue.append(Saturation);
			HSLValue.append(Luminace);
			LEDcolor.insert("HSL", HSLValue);

			item["value"] = LEDcolor;
		}
		// priorities[priorities.size()] = item;
		priorities.append(item);
	}

	info["priorities"] = priorities;
	info["priorities_autoselect"] = _hyperion->sourceAutoSelectEnabled();

	// collect adjustment information
	QJsonArray adjustmentArray;
	for (const QString& adjustmentId : _hyperion->getAdjustmentIds())
	{
		const ColorAdjustment * colorAdjustment = _hyperion->getAdjustment(adjustmentId);
		if (colorAdjustment == nullptr)
		{
			Error(_log, "Incorrect color adjustment id: %s", QSTRING_CSTR(adjustmentId));
			continue;
		}

		QJsonObject adjustment;
		adjustment["id"] = adjustmentId;

		QJsonArray whiteAdjust;
		whiteAdjust.append(colorAdjustment->_rgbWhiteAdjustment.getAdjustmentR());
		whiteAdjust.append(colorAdjustment->_rgbWhiteAdjustment.getAdjustmentG());
		whiteAdjust.append(colorAdjustment->_rgbWhiteAdjustment.getAdjustmentB());
		adjustment.insert("white", whiteAdjust);

		QJsonArray redAdjust;
		redAdjust.append(colorAdjustment->_rgbRedAdjustment.getAdjustmentR());
		redAdjust.append(colorAdjustment->_rgbRedAdjustment.getAdjustmentG());
		redAdjust.append(colorAdjustment->_rgbRedAdjustment.getAdjustmentB());
		adjustment.insert("red", redAdjust);

		QJsonArray greenAdjust;
		greenAdjust.append(colorAdjustment->_rgbGreenAdjustment.getAdjustmentR());
		greenAdjust.append(colorAdjustment->_rgbGreenAdjustment.getAdjustmentG());
		greenAdjust.append(colorAdjustment->_rgbGreenAdjustment.getAdjustmentB());
		adjustment.insert("green", greenAdjust);

		QJsonArray blueAdjust;
		blueAdjust.append(colorAdjustment->_rgbBlueAdjustment.getAdjustmentR());
		blueAdjust.append(colorAdjustment->_rgbBlueAdjustment.getAdjustmentG());
		blueAdjust.append(colorAdjustment->_rgbBlueAdjustment.getAdjustmentB());
		adjustment.insert("blue", blueAdjust);

		QJsonArray cyanAdjust;
		cyanAdjust.append(colorAdjustment->_rgbCyanAdjustment.getAdjustmentR());
		cyanAdjust.append(colorAdjustment->_rgbCyanAdjustment.getAdjustmentG());
		cyanAdjust.append(colorAdjustment->_rgbCyanAdjustment.getAdjustmentB());
		adjustment.insert("cyan", cyanAdjust);

		QJsonArray magentaAdjust;
		magentaAdjust.append(colorAdjustment->_rgbMagentaAdjustment.getAdjustmentR());
		magentaAdjust.append(colorAdjustment->_rgbMagentaAdjustment.getAdjustmentG());
		magentaAdjust.append(colorAdjustment->_rgbMagentaAdjustment.getAdjustmentB());
		adjustment.insert("magenta", magentaAdjust);

		QJsonArray yellowAdjust;
		yellowAdjust.append(colorAdjustment->_rgbYellowAdjustment.getAdjustmentR());
		yellowAdjust.append(colorAdjustment->_rgbYellowAdjustment.getAdjustmentG());
		yellowAdjust.append(colorAdjustment->_rgbYellowAdjustment.getAdjustmentB());
		adjustment.insert("yellow", yellowAdjust);

		adjustment["backlightThreshold"] = colorAdjustment->_rgbTransform.getBacklightThreshold();
		adjustment["backlightColored"]   = colorAdjustment->_rgbTransform.getBacklightColored();
		adjustment["brightness"] = colorAdjustment->_rgbTransform.getBrightness();
		adjustment["brightnessCompensation"] = colorAdjustment->_rgbTransform.getBrightnessCompensation();
		adjustment["gammaRed"]   = colorAdjustment->_rgbTransform.getGammaR();
		adjustment["gammaGreen"] = colorAdjustment->_rgbTransform.getGammaG();
		adjustment["gammaBlue"]  = colorAdjustment->_rgbTransform.getGammaB();

		adjustmentArray.append(adjustment);
	}

	info["adjustment"] = adjustmentArray;

	// collect effect info
	QJsonArray effects;
	const std::list<EffectDefinition> & effectsDefinitions = _hyperion->getEffects();
	for (const EffectDefinition & effectDefinition : effectsDefinitions)
	{
		QJsonObject effect;
		effect["name"] = effectDefinition.name;
		effect["file"] = effectDefinition.file;
		effect["script"] = effectDefinition.script;
		effect["args"] = effectDefinition.args;
		effects.append(effect);
	}

	info["effects"] = effects;

	// get available led devices
	QJsonObject ledDevices;
	ledDevices["active"] = _hyperion->getActiveDevice();
	QJsonArray availableLedDevices;
	for (auto dev: LedDevice::getDeviceMap())
	{
		availableLedDevices.append(dev.first);
	}

	ledDevices["available"] = availableLedDevices;
	info["ledDevices"] = ledDevices;

	QJsonObject grabbers;
	QJsonArray availableGrabbers;
#if defined(ENABLE_DISPMANX) || defined(ENABLE_V4L2) || defined(ENABLE_FB) || defined(ENABLE_AMLOGIC) || defined(ENABLE_OSX) || defined(ENABLE_X11)
	// get available grabbers
	//grabbers["active"] = ????;
	for (auto grabber: GrabberWrapper::availableGrabbers())
	{
		availableGrabbers.append(grabber);
	}
#endif
	grabbers["available"] = availableGrabbers;
	info["videomode"] = QString(videoMode2String(_hyperion->getCurrentVideoMode()));
	info["grabbers"]      = grabbers;

	// get available components
	QJsonArray component;
	std::map<hyperion::Components, bool> components = _hyperion->getComponentRegister().getRegister();
	for(auto comp : components)
	{
		QJsonObject item;
		item["name"] = QString::fromStdString(hyperion::componentToIdString(comp.first));
		item["enabled"] = comp.second;

		component.append(item);
	}

	info["components"] = component;
	info["imageToLedMappingType"] = ImageProcessor::mappingTypeToStr(_hyperion->getLedMappingType());

	// Add Hyperion
	QJsonObject hyperion;
	hyperion["config_modified" ] = _hyperion->configModified();
	hyperion["config_writeable"] = _hyperion->configWriteable();
	hyperion["enabled"] = _hyperion->getComponentRegister().isComponentEnabled(hyperion::COMP_ALL) ? true : false;

	info["hyperion"] = hyperion;

	// add sessions
	QJsonArray sessions;
	for (auto session: BonjourBrowserWrapper::getInstance()->getAllServices())
	{
		if (session.port<0) continue;
		QJsonObject item;
		item["name"]   = session.serviceName;
		item["type"]   = session.registeredType;
		item["domain"] = session.replyDomain;
		item["host"]   = session.hostName;
		item["address"]= session.address;
		item["port"]   = session.port;
		sessions.append(item);
	}
	info["sessions"] = sessions;

	// add plugins info
	QJsonObject plugins;
	const QMap<QString, PluginDefinition> instPlugins = _plugins->getInstalledPlugins();
	QMap<QString, PluginDefinition>::const_iterator iy = instPlugins.constBegin();
	while (iy != instPlugins.constEnd()) {

		// limit to service. plugins
		QString key = iy.key();
		if(!key.startsWith("service."))
		{
			++iy;
			continue;
		}

		QJsonObject data;
		const PluginDefinition def = iy.value();
		data["name"] = def.name;
		data["description"] = def.description;
		data["version"] = def.version;
		plugins[key] = data;

		++iy;
	}
	info["plugins"] = plugins;

	sendSuccessDataReply(QJsonDocument(info), command, tan);

	// AFTER we send the info, the client might want to subscribe to future updates
	if(message.contains("subscribe"))
	{
		// check if listeners are allowed
		if(_noListener)
			return;

		QJsonArray subsArr = message["subscribe"].toArray();
		// catch the all keyword and build a list of all cmds
		if(subsArr.contains("all"))
		{
			subsArr = QJsonArray();
			for(const auto & entry : _jsonCB->getCommands())
			{
				subsArr.append(entry);
			}
		}
		for(const auto & entry : subsArr)
		{
			// config callbacks just if auth is set
			if(entry == "settings-update" && !_authorized)
				continue;

			if(!_jsonCB->subscribeFor(entry.toString()))
				sendErrorReply(QString("Subscription for '%1' not found. Possible values: %2").arg(entry.toString(), _jsonCB->getCommands().join(", ")), command, tan);
		}
	}
}

void JsonAPI::handleClearCommand(const QJsonObject& message, const QString& command, const int tan)
{
	emit forwardJsonMessage(message);

	int priority = message["priority"].toInt();

	if(priority > 0)
		_hyperion->clear(priority);
	else if(priority < 0)
		_hyperion->clearall();
	else
	{
		sendErrorReply("Priority 0 is not allowed", command, tan);
		return;
	}

	// send reply
	sendSuccessReply(command, tan);
}

void JsonAPI::handleAdjustmentCommand(const QJsonObject& message, const QString& command, const int tan)
{
	const QJsonObject & adjustment = message["adjustment"].toObject();

	const QString adjustmentId = adjustment["id"].toString(_hyperion->getAdjustmentIds().first());
	ColorAdjustment * colorAdjustment = _hyperion->getAdjustment(adjustmentId);
	if (colorAdjustment == nullptr)
	{
		Warning(_log, "Incorrect adjustment identifier: %s", adjustmentId.toStdString().c_str());
		return;
	}

	if (adjustment.contains("red"))
	{
		const QJsonArray & values = adjustment["red"].toArray();
		colorAdjustment->_rgbRedAdjustment.setAdjustment(values[0u].toInt(), values[1u].toInt(), values[2u].toInt());
	}

	if (adjustment.contains("green"))
	{
		const QJsonArray & values = adjustment["green"].toArray();
		colorAdjustment->_rgbGreenAdjustment.setAdjustment(values[0u].toInt(), values[1u].toInt(), values[2u].toInt());
	}

	if (adjustment.contains("blue"))
	{
		const QJsonArray & values = adjustment["blue"].toArray();
		colorAdjustment->_rgbBlueAdjustment.setAdjustment(values[0u].toInt(), values[1u].toInt(), values[2u].toInt());
	}
	if (adjustment.contains("cyan"))
	{
		const QJsonArray & values = adjustment["cyan"].toArray();
		colorAdjustment->_rgbCyanAdjustment.setAdjustment(values[0u].toInt(), values[1u].toInt(), values[2u].toInt());
	}
	if (adjustment.contains("magenta"))
	{
		const QJsonArray & values = adjustment["magenta"].toArray();
		colorAdjustment->_rgbMagentaAdjustment.setAdjustment(values[0u].toInt(), values[1u].toInt(), values[2u].toInt());
	}
	if (adjustment.contains("yellow"))
	{
		const QJsonArray & values = adjustment["yellow"].toArray();
		colorAdjustment->_rgbYellowAdjustment.setAdjustment(values[0u].toInt(), values[1u].toInt(), values[2u].toInt());
	}
	if (adjustment.contains("white"))
	{
		const QJsonArray & values = adjustment["white"].toArray();
		colorAdjustment->_rgbWhiteAdjustment.setAdjustment(values[0u].toInt(), values[1u].toInt(), values[2u].toInt());
	}

	if (adjustment.contains("gammaRed"))
	{
		colorAdjustment->_rgbTransform.setGamma(adjustment["gammaRed"].toDouble(), colorAdjustment->_rgbTransform.getGammaG(), colorAdjustment->_rgbTransform.getGammaB());
	}
	if (adjustment.contains("gammaGreen"))
	{
		colorAdjustment->_rgbTransform.setGamma(colorAdjustment->_rgbTransform.getGammaR(), adjustment["gammaGreen"].toDouble(), colorAdjustment->_rgbTransform.getGammaB());
	}
	if (adjustment.contains("gammaBlue"))
	{
		colorAdjustment->_rgbTransform.setGamma(colorAdjustment->_rgbTransform.getGammaR(), colorAdjustment->_rgbTransform.getGammaG(), adjustment["gammaBlue"].toDouble());
	}

	if (adjustment.contains("backlightThreshold"))
	{
		colorAdjustment->_rgbTransform.setBacklightThreshold(adjustment["backlightThreshold"].toDouble());
	}
	if (adjustment.contains("backlightColored"))
	{
		colorAdjustment->_rgbTransform.setBacklightColored(adjustment["backlightColored"].toBool());
	}
	if (adjustment.contains("brightness"))
	{
		colorAdjustment->_rgbTransform.setBrightness(adjustment["brightness"].toInt());
	}
	if (adjustment.contains("brightnessCompensation"))
	{
		colorAdjustment->_rgbTransform.setBrightnessCompensation(adjustment["brightnessCompensation"].toInt());
	}

	// commit the changes
	_hyperion->adjustmentsUpdated();

	sendSuccessReply(command, tan);
}

void JsonAPI::handleSourceSelectCommand(const QJsonObject& message, const QString& command, const int tan)
{
	bool success = false;
	if (message["auto"].toBool(false))
	{
		_hyperion->setSourceAutoSelectEnabled(true);
		success = true;
	}
	else if (message.contains("priority"))
	{
		success = _hyperion->setCurrentSourcePriority(message["priority"].toInt());
	}

	if (success)
	{
		sendSuccessReply(command, tan);
	}
	else
	{
		sendErrorReply("setting current priority failed", command, tan);
	}
}

void JsonAPI::handleConfigCommand(const QJsonObject& message, const QString& command, const int tan)
{
	QString subcommand = message["subcommand"].toString("");
	QString full_command = command + "-" + subcommand;

	if (subcommand == "getschema")
	{
		handleSchemaGetCommand(message, full_command, tan);
	}
	else if (subcommand == "setconfig")
	{
		handleConfigSetCommand(message, full_command, tan);
	}
	else if (subcommand == "getconfig")
	{
		sendSuccessDataReply(QJsonDocument(_hyperion->getQJsonConfig()), full_command, tan);
	}
	else if (subcommand == "reload")
	{
		_hyperion->freeObjects(true);
		Process::restartHyperion();
		sendErrorReply("failed to restart hyperion", full_command, tan);
	}
	else
	{
		sendErrorReply("unknown or missing subcommand", full_command, tan);
	}
}

void JsonAPI::handleConfigSetCommand(const QJsonObject& message, const QString &command, const int tan)
{
	if (message.contains("config"))
	{
		QJsonObject config = message["config"].toObject();
		if(_hyperion->getComponentRegister().isComponentEnabled(hyperion::COMP_ALL))
		{
			if(_hyperion->saveSettings(config, true))
				sendSuccessReply(command,tan);
			else
				sendErrorReply("Failed to save configuration, more information at the Hyperion log", command, tan);
		}
		else
			sendErrorReply("Saving configuration while Hyperion is disabled isn't possible", command, tan);
	}
}

void JsonAPI::handleSchemaGetCommand(const QJsonObject& message, const QString& command, const int tan)
{
	// create result
	QJsonObject schemaJson, alldevices, properties;

	// make sure the resources are loaded (they may be left out after static linking)
	Q_INIT_RESOURCE(resource);

	// read the hyperion json schema from the resource
	QString schemaFile = ":/hyperion-schema";

	try
	{
		schemaJson = QJsonFactory::readSchema(schemaFile);
	}
	catch(const std::runtime_error& error)
	{
		throw std::runtime_error(error.what());
	}

	// collect all LED Devices
	properties = schemaJson["properties"].toObject();
	alldevices = LedDevice::getLedDeviceSchemas();
	properties.insert("alldevices", alldevices);

	// collect all available effect schemas
	QJsonObject pyEffectSchemas, pyEffectSchema;
	QJsonArray in, ex;
	const std::list<EffectSchema> & effectsSchemas = _hyperion->getEffectSchemas();
	for (const EffectSchema & effectSchema : effectsSchemas)
	{
		if (effectSchema.pyFile.mid(0, 1)  == ":")
		{
			QJsonObject internal;
			internal.insert("script", effectSchema.pyFile);
			internal.insert("schemaLocation", effectSchema.schemaFile);
			internal.insert("schemaContent", effectSchema.pySchema);
			in.append(internal);
		}
		else
		{
			QJsonObject external;
			external.insert("script", effectSchema.pyFile);
			external.insert("schemaLocation", effectSchema.schemaFile);
			external.insert("schemaContent", effectSchema.pySchema);
			ex.append(external);
		}
	}

	if (!in.empty())
		pyEffectSchema.insert("internal", in);
	if (!ex.empty())
		pyEffectSchema.insert("external", ex);

	pyEffectSchemas = pyEffectSchema;
	properties.insert("effectSchemas", pyEffectSchemas);

	schemaJson.insert("properties", properties);

	// send the result
	sendSuccessDataReply(QJsonDocument(schemaJson), command, tan);
}

void JsonAPI::handleComponentStateCommand(const QJsonObject& message, const QString &command, const int tan)
{
	const QJsonObject & componentState = message["componentstate"].toObject();

	QString compStr   = componentState["component"].toString("invalid");
	bool    compState = componentState["state"].toBool(true);

	Components component = stringToComponent(compStr);

	if (compStr == "ALL" )
	{
		if(_hyperion->getComponentRegister().setHyperionEnable(compState))
			sendSuccessReply(command, tan);
		else
			sendErrorReply(QString("Hyperion is already %1").arg(compState ? "enabled" : "disabled"), command, tan );

		return;
	}
	else if (component != COMP_INVALID)
	{
		// send result before apply
		sendSuccessReply(command, tan);
		_hyperion->setComponentState(component, compState);
		return;
	}
	sendErrorReply("invalid component name", command, tan);
}

void JsonAPI::handleLedColorsCommand(const QJsonObject& message, const QString &command, const int tan)
{
	// create result
	QString subcommand = message["subcommand"].toString("");

	if (subcommand == "ledstream-start")
	{
		_streaming_leds_reply["success"] = true;
		_streaming_leds_reply["command"] = command+"-ledstream-update";
		_streaming_leds_reply["tan"]     = tan;
		_timer_ledcolors.start(125);
	}
	else if (subcommand == "ledstream-stop")
	{
		_timer_ledcolors.stop();
	}
	else if (subcommand == "imagestream-start")
	{
		_streaming_image_reply["success"] = true;
		_streaming_image_reply["command"] = command+"-imagestream-update";
		_streaming_image_reply["tan"]     = tan;
		connect(_hyperion, &Hyperion::currentImage, this, &JsonAPI::setImage, Qt::UniqueConnection);
	}
	else if (subcommand == "imagestream-stop")
	{
		disconnect(_hyperion, &Hyperion::currentImage, this, &JsonAPI::setImage);
	}
	else
	{
		sendErrorReply("unknown subcommand \""+subcommand+"\"",command,tan);
		return;
	}

	sendSuccessReply(command+"-"+subcommand,tan);
}

void JsonAPI::handleLoggingCommand(const QJsonObject& message, const QString &command, const int tan)
{
	// create result
	QString subcommand = message["subcommand"].toString("");
	_streaming_logging_reply["success"] = true;
	_streaming_logging_reply["command"] = command;
	_streaming_logging_reply["tan"]     = tan;

	if (subcommand == "start")
	{
		if (!_streaming_logging_activated)
		{
			_streaming_logging_reply["command"] = command+"-update";
			connect(LoggerManager::getInstance(),SIGNAL(newLogMessage(Logger::T_LOG_MESSAGE)), this, SLOT(incommingLogMessage(Logger::T_LOG_MESSAGE)));
			Debug(_log, "log streaming activated for client %s",_peerAddress.toStdString().c_str()); // needed to trigger log sending
		}
	}
	else if (subcommand == "stop")
	{
		if (_streaming_logging_activated)
		{
			disconnect(LoggerManager::getInstance(), SIGNAL(newLogMessage(Logger::T_LOG_MESSAGE)), this, 0);
			_streaming_logging_activated = false;
			Debug(_log, "log streaming deactivated for client  %s",_peerAddress.toStdString().c_str());

		}
	}
	else
	{
		sendErrorReply("unknown subcommand",command,tan);
		return;
	}

	sendSuccessReply(command+"-"+subcommand,tan);
}

void JsonAPI::handleProcessingCommand(const QJsonObject& message, const QString &command, const int tan)
{
	_hyperion->setLedMappingType(ImageProcessor::mappingTypeToInt( message["mappingType"].toString("multicolor_mean")) );

	sendSuccessReply(command, tan);
}

void JsonAPI::handleVideoModeCommand(const QJsonObject& message, const QString &command, const int tan)
{
	_hyperion->setVideoMode(parse3DMode(message["videoMode"].toString("2D")));

	sendSuccessReply(command, tan);
}

void JsonAPI::handleAuthorizeCommand(const QJsonObject & message, const QString &command, const int tan)
{
	const QString& subc = message["subcommand"].toString().trimmed();
	// catch test if auth is required
	if(subc == "required")
	{
		QJsonObject req;
		req["required"] = _apiAuthRequired;
		sendSuccessDataReply(QJsonDocument(req), command+"-"+subc, tan);
		return;
	}

	// catch logout
	if(subc == "logout")
	{
		_authorized = false;
		_userAuthorized = false;
		sendSuccessReply(command+"-"+subc, tan);
		return;
	}

	// token created from ui
	if(subc == "createToken")
	{
		const QString& c = message["comment"].toString().trimmed();
		// for user authorized sessions
		if(_userAuthorized)
		{
			AuthManager::AuthDefinition def = _authManager->createToken(c);
			QJsonObject newTok;
			newTok["comment"] = def.comment;
			newTok["id"] = def.id;
			newTok["token"] = def.token;

			sendSuccessDataReply(QJsonDocument(newTok), command+"-"+subc, tan);
			return;
		}
		sendErrorReply("No Authorization",command+"-"+subc, tan);
		return;
	}

	// delete token
	if(subc == "deleteToken")
	{
		const QString& did = message["id"].toString().trimmed();
		// for user authorized sessions
		if(_userAuthorized)
		{
			_authManager->deleteToken(did);
			sendSuccessReply(command+"-"+subc, tan);
			return;
		}
		sendErrorReply("No Authorization",command+"-"+subc, tan);
		return;
	}

	// catch token request
	if(subc == "requestToken")
	{
		const QString& comment = message["comment"].toString().trimmed();
		const QString& id = message["id"].toString().trimmed();
		if(comment.count() >= 10 && id.count() == 5)
		{
			_authManager->setNewTokenRequest(this, comment, id);
			// client should wait for answer
		}
		else
			sendErrorReply("Comment is too short or id length wrong", command+"-"+subc, tan);

		return;
	}

	// get pending token requests
	if(subc == "getPendingRequests")
	{
		if(_userAuthorized)
		{
			QMap<QString, AuthManager::AuthDefinition> map = _authManager->getPendingRequests();
			QJsonArray arr;
			for(const auto& entry : map)
			{
				QJsonObject obj;
				obj["comment"] = entry.comment;
				obj["id"] = entry.id;
				obj["timeout"] = int(entry.timeoutTime - QDateTime::currentMSecsSinceEpoch());
				arr.append(obj);
			}
			sendSuccessDataReply(QJsonDocument(arr),command+"-"+subc, tan);
		}
		else
			sendErrorReply("No Authorization", command+"-"+subc, tan);

		return;
	}

	// accept token request
	if(subc == "answerRequest")
	{
		const QString& id = message["id"].toString().trimmed();
		const bool& accept = message["accept"].toBool(false);
		if(_userAuthorized)
		{
			if(accept)
				_authManager->acceptTokenRequest(id);
			else
				_authManager->denyTokenRequest(id);
		}
		else
			sendErrorReply("No Authorization", command+"-"+subc, tan);

		return;
	}
	// deny token request
	if(subc == "acceptRequest")
	{
		const QString& id = message["id"].toString().trimmed();
		if(_userAuthorized)
		{
			_authManager->acceptTokenRequest(id);
		}
		else
			sendErrorReply("No Authorization", command+"-"+subc, tan);

		return;
	}

	// cath get token list
	if(subc == "getTokenList")
	{
		if(_userAuthorized)
		{
			QVector<AuthManager::AuthDefinition> defVect = _authManager->getTokenList();
			QJsonArray tArr;
			for(const auto& entry : defVect)
			{
				QJsonObject subO;
				subO["comment"] = entry.comment;
				subO["id"] = entry.id;
				subO["last_use"] = entry.lastUse;

				tArr.append(subO);
			}

			sendSuccessDataReply(QJsonDocument(tArr),command+"-"+subc, tan);
			return;
		}
		sendErrorReply("No Authorization",command+"-"+subc, tan);
		return;
	}

	// login
	if(subc == "login")
	{
		// catch token auth
		const QString& token = message["token"].toString().trimmed();

		if(!token.isEmpty())
		{
			if(token.count() >= 36)
			{
				if(_authManager->isTokenAuthorized(token))
				{
					_authorized = true;
					sendSuccessReply(command+"-"+subc, tan);
				}
				else
					sendErrorReply("No Authorization", command+"-"+subc, tan);
			}
			else
				sendErrorReply("Token is too short", command+"-"+subc, tan);

			return;
		}

		// user & password
		const QString& user = message["username"].toString().trimmed();
		const QString& password = message["password"].toString().trimmed();

		if(user.count() >= 3 && password.count() >= 8)
		{
			if(_authManager->isUserAuthorized(user, password))
			{
				_authorized = true;
				_userAuthorized = true;
				sendSuccessReply(command+"-"+subc, tan);
			}
			else
				sendErrorReply("No Authorization", command+"-"+subc, tan);
		}
		else
			sendErrorReply("User or password string too short", command+"-"+subc, tan);
	}
}

const bool JsonAPI::handleHTTPAuth(const QString& command, const int& tan, const QString& token)
{
	if(_authManager->isTokenAuthorized(token))
	{
		_authorized = true;
		return true;
	}
	sendErrorReply("No Authorization", command, tan);
	return false;
}

void JsonAPI::handlePluginCommand(const QJsonObject& message, const QString &command, const int tan)
{
	const QString& subc = message["subcommand"].toString();
	const QString full_command = command+"-"+subc;
	const QString& id = message["id"].toString();
	const bool& state = message["state"].toBool();

	// some actions are just available with auth
	QStringList restrC;
	restrC << "install" << "remove" << "autoupdate" << "save" << "getInitData" << "updateavail";
	if(restrC.contains(subc) && !_authorized)
	{
		sendErrorReply("No authorization", command+"-"+subc, tan);
		return;
	}

	if(subc == "getInitData")
	{
		QJsonObject result;
		QJsonObject obj;
		const QMap<QString, PluginDefinition> availP = _plugins->getAvailablePlugins();
		const QMap<QString, PluginDefinition> instP = _plugins->getInstalledPlugins();
		QMap<QString, PluginDefinition>::const_iterator i = instP.constBegin();
		while (i != instP.constEnd()) {

			QJsonObject data;
			const PluginDefinition def = i.value();
			data["name"] = def.name;
			data["description"] = def.description;
			data["version"] = def.version;
			data["dependencies"] = def.dependencies;
			data["changelog"] = def.changelog;
			data["provider"] = def.provider;
			data["support"] = def.support;
			data["source"] = def.source;
			data["settingsSchema"] = def.settingsSchema;
			data["settings"] = def.settings;
			data["running"] = _plugins->isPluginRunning(i.key());
			data["autoupdate"] = _plugins->isPluginAutoUpdateEnabled(i.key());
			obj[i.key()] = data;

			++i;
		}
		result["installedPlugins"] = obj;
		QJsonObject obj2;

		QMap<QString, PluginDefinition>::const_iterator ix = availP.constBegin();
		while (ix != availP.constEnd()) {

			QJsonObject data;
			const PluginDefinition def = ix.value();
			data["name"] = def.name;
			data["description"] = def.description;
			data["version"] = def.version;
			data["dependencies"] = def.dependencies;
			data["changelog"] = def.changelog;
			data["provider"] = def.provider;
			data["support"] = def.support;
			data["source"] = def.source;
			obj2[ix.key()] = data;

			++ix;
		}
		result["availablePlugins"] = obj2;
		sendSuccessDataReply(QJsonDocument(result), full_command, tan);
	}
	else if(subc == "start")
	{
		emit pluginAction(P_START, id);
	}
	else if(subc == "stop")
	{
		emit pluginAction(P_STOP, id);
	}
	else if(subc == "install")
	{
		emit pluginAction(P_INSTALL, id);
	}
	else if(subc == "remove")
	{
		emit pluginAction(P_REMOVE, id);
	}
	else if(subc == "autoupdate")
	{
		emit pluginAction(P_AUTOUPDATE, id, state);
	}
	else if(subc == "save")
	{
		const QJsonObject& data = message["data"].toObject();
		PluginDefinition newDef;
		newDef.settings = data;
		emit pluginAction(P_SAVE, id, true, newDef);
	}
	else if(subc == "updateavail")
	{
		emit pluginAction(P_UPD_AVAIL, id, state);
	}
	else
	{
		sendErrorReply("Not implemented", full_command, tan);
	}
}

void JsonAPI::doPluginAction(PluginAction action, QString id, bool success, PluginDefinition def)
{
	QList<PluginAction> restrA;
	restrA << P_SAVED << P_INSTALLED << P_REMOVED << P_AUTOUPDATED << P_UPDATED_AVAIL;
	if(restrA.contains(action) && !_authorized)
		return;

	QJsonObject result;
	QString cmd;
	bool send = true;
	result["success"] = success;
	result["id"] = id;

	QJsonObject data;

	switch(action)
	{
		case P_STARTED:
			cmd = "plugin-start";
			break;
		case P_STOPPED:
			cmd = "plugin-stop";
			break;
		case P_ERROR:
			cmd = "plugin-error"; // implicit stop, but crashed
			break;
		case P_INSTALLED:
			cmd = "plugin-install";
			if(success)
			{
				data["name"] = def.name;
				data["description"] = def.description;
				data["version"] = def.version;
				data["dependencies"] = def.dependencies;
				data["changelog"] = def.changelog;
				data["provider"] = def.provider;
				data["support"] = def.support;
				data["source"] = def.source;
				data["settingsSchema"] = def.settingsSchema;
				data["settings"] = def.settings;
				data["running"] = _plugins->isPluginRunning(id);
				data["autoupdate"] = _plugins->isPluginAutoUpdateEnabled(id);

				result["data"] = data;
			}
			break;
		case P_REMOVED:
			cmd = "plugin-remove";
			break;
		case P_SAVED:
			cmd = "plugin-save";
			result["data"] = def.settings;
			break;
		case P_AUTOUPDATED:
			cmd = "plugin-autoupdate";
			break;
		case P_UPDATED_AVAIL:
			cmd = "plugin-updateavail";
			if(success)
			{
				const QMap<QString, PluginDefinition> availP = _plugins->getAvailablePlugins();
				QMap<QString, PluginDefinition>::const_iterator ix = availP.constBegin();
				QJsonObject obj;
				while (ix != availP.constEnd()) {
					QJsonObject data;
					const PluginDefinition def = ix.value();
					data["name"] = def.name;
					data["description"] = def.description;
					data["version"] = def.version;
					data["dependencies"] = def.dependencies;
					data["changelog"] = def.changelog;
					data["provider"] = def.provider;
					data["support"] = def.support;
					data["source"] = def.source;
					obj[ix.key()] = data;

					++ix;
				}
				result["data"] = obj;
			}
			break;
		default:
			send = false;
			break;
	}

	if(send)
	{
		sendSuccessDataReply(QJsonDocument(result), cmd);
	}
}

void JsonAPI::handleNotImplemented()
{
	sendErrorReply("Command not implemented");
}

void JsonAPI::sendSuccessReply(const QString &command, const int tan)
{
	// create reply
	QJsonObject reply;
	reply["success"] = true;
	reply["command"] = command;
	reply["tan"] = tan;

	// send reply
	emit callbackMessage(reply);
}

void JsonAPI::sendSuccessDataReply(const QJsonDocument &doc, const QString &command, const int &tan)
{
	QJsonObject reply;
	reply["success"] = true;
	reply["command"] = command;
	reply["tan"] = tan;
	if(doc.isArray())
		reply["info"] = doc.array();
	else
		reply["info"] = doc.object();

	emit callbackMessage(reply);
}

void JsonAPI::sendErrorReply(const QString &error, const QString &command, const int tan)
{
	// create reply
	QJsonObject reply;
	reply["success"] = false;
	reply["error"] = error;
	reply["command"] = command;
	reply["tan"] = tan;

	// send reply
	emit callbackMessage(reply);
}


void JsonAPI::streamLedcolorsUpdate()
{
	QJsonObject result;
	QJsonArray leds;

	const std::vector<ColorRgb> & ledColors = _hyperion->getRawLedBuffer();
	for(auto color = ledColors.begin(); color != ledColors.end(); ++color)
	{
		QJsonObject item;
		item["index"] = int(color - ledColors.begin());
		item["red"]   = color->red;
		item["green"] = color->green;
		item["blue"]  = color->blue;
		leds.append(item);
	}

	result["leds"] = leds;
	_streaming_leds_reply["result"] = result;

	// send the result
	emit callbackMessage(_streaming_leds_reply);
}

void JsonAPI::setImage(const Image<ColorRgb> & image)
{
	if ( (_image_stream_timeout+250) < QDateTime::currentMSecsSinceEpoch() && _image_stream_mutex.tryLock(0) )
	{
		_image_stream_timeout = QDateTime::currentMSecsSinceEpoch();

		QImage jpgImage((const uint8_t *) image.memptr(), image.width(), image.height(), 3*image.width(), QImage::Format_RGB888);
		QByteArray ba;
		QBuffer buffer(&ba);
		buffer.open(QIODevice::WriteOnly);
		jpgImage.save(&buffer, "jpg");

		QJsonObject result;
		result["image"] = "data:image/jpg;base64,"+QString(ba.toBase64());
		_streaming_image_reply["result"] = result;
		emit callbackMessage(_streaming_image_reply);

		_image_stream_mutex.unlock();
	}
}

void JsonAPI::incommingLogMessage(Logger::T_LOG_MESSAGE msg)
{
	QJsonObject result, message;
	QJsonArray messageArray;

	if (!_streaming_logging_activated)
	{
		_streaming_logging_activated = true;
		QVector<Logger::T_LOG_MESSAGE>* logBuffer = LoggerManager::getInstance()->getLogMessageBuffer();
		for(int i=0; i<logBuffer->length(); i++)
		{
			message["appName"] = logBuffer->at(i).appName;
			message["loggerName"] = logBuffer->at(i).loggerName;
			message["function"] = logBuffer->at(i).function;
			message["line"] = QString::number(logBuffer->at(i).line);
			message["fileName"] = logBuffer->at(i).fileName;
			message["message"] = logBuffer->at(i).message;
			message["levelString"] = logBuffer->at(i).levelString;

			messageArray.append(message);
		}
	}
	else
	{
		message["appName"] = msg.appName;
		message["loggerName"] = msg.loggerName;
		message["function"] = msg.function;
		message["line"] = QString::number(msg.line);
		message["fileName"] = msg.fileName;
		message["message"] = msg.message;
		message["levelString"] = msg.levelString;

		messageArray.append(message);
	}

	result.insert("messages", messageArray);
	_streaming_logging_reply["result"] = result;

	// send the result
	emit callbackMessage(_streaming_logging_reply);
}

void JsonAPI::handlePendingTokenRequest(const QString& id, const QString& comment)
{
	// just user sessions are allowed to react on this, to prevent that token authorized instances authorize new tokens on their own
	if(_userAuthorized)
	{
		QJsonObject obj;
		obj["command"] = "authorize-event";
		obj["comment"] = comment;
		obj["id"] = id;

		emit callbackMessage(obj);
	}
}

void JsonAPI::handleTokenResponse(const bool& success, QObject* caller, const QString& token, const QString& comment, const QString& id)
{
	// if this is the requester, we send the reply
	if(this == caller)
	{
		const QString cmd = "authorize-requestToken";
		QJsonObject result;
		result["token"] = token;
		result["comment"] = comment;
		result["id"] = id;

		if(success)
			sendSuccessDataReply(QJsonDocument(result), cmd);
		else
			sendErrorReply("Token request timeout or denied", cmd);
	}
}
