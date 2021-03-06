#pragma once

// Python
#undef slots
#include <Python.h>
#define slots

//hyperion incl
#include <utils/Logger.h>
#include <plugin/PluginDefinition.h>
#include <utils/ColorRgb.h>
#include <utils/Components.h>
#include <hyperion/PriorityMuxer.h>

// qt incl
#include <QThread>
#include <QStringList>

class Hyperion;
class Plugins;

class Plugin : public QThread
{
	Q_OBJECT

public:

	Plugin(Plugins* plugins, Hyperion* hyperion, const PluginDefinition& def, const QString& id, const QStringList& dPaths);
	virtual ~Plugin();

	// QThread inherited run method
	virtual void run();

	/// get plugin id
	const QString getId(){ return _id; };
	/// is true when a error occurred
	bool hasError() const { return _error; };
	/// set the remove flag
	void setRemoveFlag(){ _remove = true; };
	/// get the remove flag
	bool hasRemoveFlag() const { return _remove; };

private:
	PyThreadState* _state;
	/// The plugins instance
	Plugins* _plugins;
	/// Instance pointer of Hyperion
	Hyperion* _hyperion;
	/// prio muxer
	PriorityMuxer* _prioMuxer;
	/// definition of this instance
	PluginDefinition _def;
	/// id of the plugin
	const QString _id;
	/// dependencies Paths
	const QStringList _dPaths;
	/// Logger instance
	Logger *_log;
	/// store py path
	std::string _pythonPath;
	/// true if error occurred
	bool _error = false;
	/// true if plugin should be removed after stop
	bool _remove = false;

	// add a python path to _pythonPath
	void addNativePath(const std::string& path);

	// build _pythonPath and apply with dependencies
	void handlePyPath(void);

	// prints exception to log
	void printException(void);

	FILE* PyFile_AsFileWithMode(PyObject *py_file, const char *mode);

public:
	/// Map of all callback PyObjects that has been requested by this instance
	QMap<QString, PyObject*> callbackObjects;

public:
	///
	/// INSTANCE METHODS FOR PYTHON PLUGIN MODUL METHODS
	///

	///
	/// @brief Print a log message from plugin
	/// @param msg  The message to print
	/// @param lvl  The log lvl. See loglvlEnum in Plugin module, unhandled value is Debug
	///
	void printToLog(char* msg, int lvl = -1);

	///
	/// @brief Get the current settings object for this plugin instance
	/// @return The settings
	///
	const QJsonValue getSettings();

	///
	/// @brief Set a component state of a specific component
	/// @param comp   The comp to check
	/// @return -1 when comp is not found, True(1) on comp enabled else False
	///
	const int getComponentState(int comp);

	///
	/// @brief Set a component state of a specific component
	/// @param comp   See available comps at Module v
	/// @param enable If true it enables the comp, else false
	/// @return True if component was found else false
	///
	const int setComponentState(int comp, const int& enable);

	///
	/// @brief Set a single color with a specific timeout
	/// @param  color   The RGB color
	/// @param  priority The priority channel
	/// @param  duration  The duration
	///
	void setColor(const ColorRgb& color, const int& priority, const int& duration);

	///
	/// @brief Set a effect by name, at the given priority with duration
	/// @param  name      The name of the effect
	/// @param  priority  The priority
	/// @param  duration  duration in ms
	///
	int setEffect(const char* name, const int& priority, const int& duration);

	///
	/// @brief Get priority info for given priority
	/// @return         The priority
	///
	PriorityMuxer::InputInfo getPriorityInfo(const int& priority) const;

	///
	/// @brief Get all priorities in PriorityMuxer
	/// @return prios 
	///
	QList<int> getAllPriorities() const;

	///
	/// @brief Get the current visible priority
	/// @return         The priority
	///
	int getVisiblePriority() const;

	///
	/// @brief Set the given priority to visible
	/// @param priority The priority to set
	/// @return         True on success, false if not found
	///
	int setVisiblePriority(const int& priority);

public slots:
	///
	/// CALLBACK SLOTS
	///

	/// @brief called whenever a plugin action is ongoing
	/// @param action   action from enum
	/// @param id       plugin id
	/// @param def      PluginDefinition (optional)
	/// @param success  true if action was a success, else false
	///
	void handlePluginAction(PluginAction action, QString id, bool success = true, PluginDefinition def = PluginDefinition());

	///
	/// @brief called when a component state is changed
	/// @param comp   the component
	/// @param state	the changed state
	///
	void onCompStateChanged(const hyperion::Components comp, bool state);

	///
	/// @brief called when the visible priorty has changed
	/// @param priority   the prioriry
	///
	void onVisiblePriorityChanged(const quint8& priority);
};
