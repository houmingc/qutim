/****************************************************************************
**
** qutIM - instant messenger
**
** Copyright © 2011 Ruslan Nigmatullin <euroelessar@yandex.ru>
**
*****************************************************************************
**
** $QUTIM_BEGIN_LICENSE$
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
** See the GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program.  If not, see http://www.gnu.org/licenses/.
** $QUTIM_END_LICENSE$
**
****************************************************************************/

#include "config.h"
#include "cryptoservice.h"
#include "systeminfo.h"
#include "metaobjectbuilder.h"
#include "debug.h"
#include <QSet>
#include <QStringList>
#include <QFileInfo>
#include <QDateTime>
#include <QEvent>
#include <QCoreApplication>
#include <QBasicTimer>
#include <QSharedPointer>

#define CONFIG_MAKE_DIRTY_ONLY_AT_SET_VALUE 1

namespace qutim_sdk_0_3
{
Q_GLOBAL_STATIC(QList<ConfigBackend*>, all_config_backends)
LIBQUTIM_EXPORT QList<ConfigBackend*> &get_config_backends()
{ return *all_config_backends(); }

class ConfigAtom
{
public:
    typedef QSharedPointer<ConfigAtom> Ptr;
    typedef QMap<QString, Ptr> ConfigMap;
    typedef QVector<Ptr> ConfigList;
    typedef QVariant ConfigValue;

    enum Type {
        List,
        Map,
        Value,
        Null
    };

    ConfigAtom(bool readOnly) : m_type(Null), m_readOnly(readOnly)
    {
    }

    ~ConfigAtom()
    {
        clear();
    }

    static Ptr fromVariant(const QVariant &variant, bool readOnly)
    {
        auto result = Ptr::create(readOnly);

        // Null value
        if (!variant.isValid())
            return result;

        switch (variant.type()) {
        case QVariant::Map: {
            const auto &input = variant.toMap();
            auto &map = result->ensureMap();
            for (auto it = input.begin(); it != input.end(); ++it)
                map.insert(it.key(), fromVariant(it.value(), readOnly));
        }
            break;
        case QVariant::List: {
            const auto &input = variant.toList();
            auto &list = result->ensureList();
            list.reserve(input.size());
            for (const auto &value : input)
                list.append(fromVariant(value, readOnly));
        }
            break;
        default:
            result->ensureValue() = variant;
            break;
        }

        return result;
    }

    bool isReadOnly() const
    {
        return m_readOnly;
    }

    bool isMap() const
    {
        return m_type == Map;
    }

    bool isList() const
    {
        return m_type == List;
    }

    bool isValue() const
    {
        return m_type == Value;
    }

    bool isNull() const
    {
        return m_type == Null;
    }

    Type type() const
    {
        return m_type;
    }

    QVariant toVariant()
    {
        switch (m_type) {
        case Map: {
            QVariantMap result;
            const auto &map = asMap();
            for (auto it = map.begin(); it != map.end(); ++it)
                result.insert(it.key(), it.value()->toVariant());
            return result;
        }
        case List: {
            QVariantList result;
            const auto &list = asList();
            for (const auto &value : list)
                result.append(value->toVariant());
            return result;
        }
        case Value:
            return asValue();
        case Null:
            return QVariant();
        }
    }

    Ptr child(const QString &name)
    {
        Q_ASSERT(!m_readOnly || isMap());
        auto &map = ensureMap();

        auto it = map.find(name);
        if (it == map.end()) {
            if (m_readOnly)
                return Ptr();

            it = map.insert(name, ConfigAtom::Ptr::create(m_readOnly));
        }

        return it.value();
    }

    Ptr child(int index)
    {
        Q_ASSERT(!m_readOnly || isList());
        auto &list = ensureList();

        if (m_readOnly && list.size() <= index)
            return Ptr();

        while (list.size() <= index)
            list.append(ConfigAtom::Ptr::create(m_readOnly));

        return list.at(index);
    }

    int arraySize()
    {
        Q_ASSERT(isList());
        auto &list = asList();
        return list.size();
    }

    template <typename Callback>
    void iterateChildren(Callback callback)
    {
        if (isMap()) {
            for (auto value : asMap())
                callback(value);
        } else if (isList()) {
            for (auto value : asList())
                callback(value);
        }
    }

    template <typename Callback>
    void iterateMap(Callback callback)
    {
        Q_ASSERT(isMap());

        auto &map = asMap();
        for (auto it = map.begin(); it != map.end(); ++it)
            callback(it.key(), it.value());
    }

    bool remove(int index)
    {
        Q_ASSERT(isList());
        auto &list = asList();
        if (list.size() <= index)
            return false;

        list.remove(index);
        return true;
    }

    bool remove(const QString &name)
    {
        Q_ASSERT(isMap());
        return asMap().remove(name) > 0;
    }

    bool replace(const QString &name, const ConfigAtom::Ptr &value)
    {
        Q_ASSERT(isMap());
        bool dirty = false;

        ConfigAtom::ConfigMap &map = asMap();
        if (!map.contains(name) || map[name]->toVariant() != value->toVariant()) {
            map[name] = value;
            dirty = true;
        }

        return dirty;
    }

    void convert(Type type)
    {
        if (m_type == type)
            return;

        switch (type) {
        case Map:
            ensureMap();
            break;
        case List:
            ensureList();
            break;
        case Value:
            ensureValue();
            break;
        case Null:
            clear();
            break;
        }
    }

private:
    ConfigMap &asMap()
    {
        Q_ASSERT(isMap());
        return ensureMap();
    }

    ConfigList &asList()
    {
        Q_ASSERT(isList());
        return ensureList();
    }

    ConfigValue &asValue()
    {
        Q_ASSERT(isValue());
        return ensureValue();
    }

    ConfigMap &ensureMap()
    {
        auto map = data<ConfigMap>();

        if (m_type != Map) {
            clear();
            new (map) ConfigMap();
            m_type = Map;
        }

        return *map;
    }

    ConfigList &ensureList()
    {
        auto list = data<ConfigList>();

        if (m_type != List) {
            clear();
            new (list) ConfigList();
            m_type = List;
        }

        return *list;
    }

    ConfigValue &ensureValue()
    {
        auto value = data<ConfigValue>();

        if (m_type != Value) {
            clear();
            new (value) ConfigValue();
            m_type = Value;
        }

        return *value;
    }

private:
    void clear()
    {
        switch (m_type) {
        case Map:
            data<ConfigMap>()->~ConfigMap();
            break;
        case List:
            data<ConfigList>()->~ConfigList();
            break;
        case Value:
            data<ConfigValue>()->~ConfigValue();
            break;
        case Null:
            break;
        }

        m_type = Null;
    }

    template <typename T>
    T *data()
    {
        char *buffer = m_buffer;
        return reinterpret_cast<T *>(buffer);
    }

    union {
        char m_map_buffer[sizeof(ConfigMap)];
        char m_list_buffer[sizeof(ConfigList)];
        char m_value_buffer[sizeof(ConfigValue)];
        char m_buffer[1];
    };
    Type m_type;
    const bool m_readOnly;
};

class ConfigSource
{
	Q_DISABLE_COPY(ConfigSource)
public:
    typedef QSharedPointer<ConfigSource> Ptr;

    inline ConfigSource() : backend(nullptr), dirty(false), isAtLoop(false)
	{
	}
	inline ~ConfigSource()
	{
		if (dirty) sync();
	}

	static ConfigSource::Ptr open(const QString &path, bool systemDir, bool create, ConfigBackend *bcknd = 0);
	inline void update() { lastModified = QFileInfo(fileName).lastModified(); }
	bool isValid() {
        return QFileInfo(fileName).lastModified() == lastModified;
	}
	void sync();
    void makeDirty() { dirty = true; }
	QString fileName;
	ConfigBackend *backend;
	bool dirty;
	bool isAtLoop;
    ConfigAtom::Ptr data;
	QDateTime lastModified;
};

class ConfigSourceHash : public QObject
{
public:
	ConfigSource::Ptr value(const QString &key)
	{
        auto it = m_hash.find(key);
		if (it == m_hash.end())
			return ConfigSource::Ptr();

		m_timers.remove(it->timer.timerId());
		it->timer.start(5 * 60, this);
		m_timers.insert(it->timer.timerId(), key);

		return it->config;
	}

	void insert(const QString &key, const ConfigSource::Ptr &value)
	{
        auto it = m_hash.find(key);
		if (it == m_hash.end())
			it = m_hash.insert(key, Info());

		m_timers.remove(it->timer.timerId());
        it->timer.start(5 * 60, this);
		m_timers.insert(it->timer.timerId(), key);

        it->config = value;
	}

	void timerEvent(QTimerEvent *event)
	{
		QString key = m_timers.take(event->timerId());
		m_hash.remove(key);
	}

private:
	struct Info
	{
		mutable QBasicTimer timer;
		ConfigSource::Ptr config;
	};

	typedef QHash<QString, Info> Hash;
	Hash m_hash;
	mutable QHash<int, QString> m_timers;
};

Q_GLOBAL_STATIC(ConfigSourceHash, sourceHash)

ConfigSource::Ptr ConfigSource::open(const QString &path, bool systemDir, bool create, ConfigBackend *backend)
{
	QString fileName = path;
	if (fileName.isEmpty())
		fileName = QLatin1String("profile");
	QFileInfo info(fileName);
	if (!info.isAbsolute()) {
		SystemInfo::DirType type = systemDir
				? SystemInfo::SystemConfigDir
				: SystemInfo::ConfigDir;
		fileName = SystemInfo::getDir(type).filePath(fileName);
	} else if (systemDir) {
		// We need to open absolute dir only once
		return ConfigSource::Ptr();
	}
	fileName = QDir::cleanPath(fileName);

	ConfigSource::Ptr result = sourceHash()->value(fileName);
	if (result && result->isValid())
		return result;
	
	const QList<ConfigBackend*> &backends = *all_config_backends();
	if (!backend) {
		QByteArray suffix = info.suffix().toLatin1().toLower();
	
		if (backends.isEmpty())
			return ConfigSource::Ptr();
	
		if (!suffix.isEmpty()) {
			for (int i = 0; i < backends.size(); i++) {
				if (backends.at(i)->name() == suffix) {
					backend = backends.at(i);
					break;
				}
			}
		}
		if (!backend) {
            backend = backends.first();
			fileName += QLatin1Char('.');
			fileName += QLatin1String(backend->name());
	
			result = sourceHash()->value(fileName);
			if (result && result->isValid())
				return result;
			info.setFile(fileName);
		}
	}

	if (!info.exists() && !create)
		return result;

	QDir dir = info.absoluteDir();
	if (!dir.exists()) {
		if (!create)
			return result;
		dir.mkpath(info.absolutePath());
	}

    result = ConfigSource::Ptr::create();

	ConfigSource *d = result.data();
	d->backend = backend;
	d->fileName = fileName;
	// QFileInfo says that we can't write to non-exist files but we can
    const bool readOnly = !info.isWritable() && (systemDir || info.exists());

	d->update();
    const QVariant value = d->backend->load(d->fileName);
    d->data = ConfigAtom::fromVariant(value, readOnly);

    if (d->data->isValue() || d->data->isNull()) {
        if (!create)
            return ConfigSource::Ptr();

        d->data = ConfigAtom::fromVariant(QVariantMap(), readOnly);
    }

	sourceHash()->insert(fileName, result);
	return result;
}

void ConfigSource::sync()
{
    backend->save(fileName, data->toVariant());
    dirty = false;
    update();
}

class PostConfigSaveEvent : public QEvent
{
public:
	PostConfigSaveEvent(const ConfigSource::Ptr &s) : QEvent(eventType()), source(s) {}
	static Type eventType()
	{
		static Type type = static_cast<Type>(registerEventType());
		return type;
	}
	ConfigSource::Ptr source;
};

class PostConfigSaver : public QObject
{
public:
	PostConfigSaver()
	{
		qAddPostRoutine(cleanup);
	}

	virtual bool event(QEvent *ev)
	{
		if (ev->type() == PostConfigSaveEvent::eventType()) {
			PostConfigSaveEvent *saveEvent = static_cast<PostConfigSaveEvent*>(ev);
			saveEvent->source->sync();
			saveEvent->source->isAtLoop = false;
			return true;
		}
		return QObject::event(ev);
	}
private:
	static void cleanup();
};

Q_GLOBAL_STATIC(PostConfigSaver, postConfigSaver)

void PostConfigSaver::cleanup()
{
	QCoreApplication::sendPostedEvents(postConfigSaver(), PostConfigSaveEvent::eventType());
}

class ConfigLevel
{
public:
    typedef QSharedPointer<ConfigLevel> Ptr;

    inline ConfigLevel() : arrayElement(false)
    {
    }
    inline ConfigLevel(const QList<ConfigAtom::Ptr> &atoms) : atoms(atoms), arrayElement(false)
    {
    }
    inline ~ConfigLevel()
    {
    }

    ConfigLevel::Ptr child(int index);
    ConfigLevel::Ptr child(const QString &name);
    ConfigLevel::Ptr child(const QList<QString> &names);

    template <typename Callback>
    void iterateChildren(Callback callback);
    template <typename Callback>
    void iterateMap(Callback callback);

    ConfigLevel::Ptr convert(ConfigAtom::Type type);

    QList<ConfigAtom::Ptr> atoms;
    bool arrayElement;

private:
    template <typename Callback>
    ConfigLevel::Ptr map(Callback callback);
};

ConfigLevel::Ptr ConfigLevel::child(int index)
{
    return map([index] (const ConfigAtom::Ptr &atom, bool readOnly) {
        if (readOnly && !atom->isList())
            return ConfigAtom::Ptr();
        return atom->child(index);
    });
}

ConfigLevel::Ptr ConfigLevel::child(const QString &name)
{
    return map([name] (const ConfigAtom::Ptr &atom, bool readOnly) {
        if (readOnly && !atom->isMap())
            return ConfigAtom::Ptr();
        return atom->child(name);
    });
}

ConfigLevel::Ptr ConfigLevel::child(const QList<QString> &names)
{
    Q_ASSERT(!names.isEmpty());

    ConfigLevel::Ptr level = child(names.first());
    for (int i = 1; i < names.size(); ++i)
        level = level->child(names[i]);

    return level;
}

template <typename Callback>
void ConfigLevel::iterateChildren(Callback callback)
{
    for (const ConfigAtom::Ptr &atom : atoms) {
        atom->iterateChildren(callback);
    }
}

template <typename Callback>
void ConfigLevel::iterateMap(Callback callback)
{
    for (const ConfigAtom::Ptr &atom : atoms) {
        if (atom->isMap())
            atom->iterateMap(callback);
    }
}

ConfigLevel::Ptr ConfigLevel::convert(ConfigAtom::Type type)
{
    return map([type] (const ConfigAtom::Ptr &atom, bool readOnly) -> ConfigAtom::Ptr {
        if (readOnly && atom->type() != type)
            return ConfigAtom::Ptr();

        if (atom->type() != type)
            atom->convert(type);

        return atom;
    });
}

template <typename Callback>
ConfigLevel::Ptr ConfigLevel::map(Callback callback)
{
    auto level = ConfigLevel::Ptr::create();
    bool first = true;
    QList<ConfigAtom::Ptr> &results = level->atoms;

    for (const ConfigAtom::Ptr &atom : atoms) {
        const bool isReadOnly = atom->isReadOnly() || !first;
        first = false;

        if (auto result = callback(atom, isReadOnly))
            results << result;
    }

    return level;
}

class ConfigPrivate : public QSharedData
{
	Q_DISABLE_COPY(ConfigPrivate)
public:
    ConfigPrivate();
    ConfigPrivate(const QStringList &paths, ConfigBackend *backend = 0);
    ConfigPrivate(const QStringList &paths, const QVariantList &fallbacks, ConfigBackend *backend = nullptr);
    ~ConfigPrivate();

    inline const ConfigLevel::Ptr &current() const { return levels.first(); }

    void sync();
    QExplicitlySharedDataPointer<ConfigPrivate> clone();

	QList<ConfigLevel::Ptr> levels;
	QList<ConfigSource::Ptr> sources;

	QExplicitlySharedDataPointer<ConfigPrivate> memoryGuard;
};

ConfigPrivate::ConfigPrivate()
{
    levels << ConfigLevel::Ptr::create();
}

ConfigPrivate::ConfigPrivate(const QStringList &paths, ConfigBackend *backend)
    : ConfigPrivate(paths, QVariantList(), backend)
{
}

ConfigPrivate::ConfigPrivate(const QStringList &paths, const QVariantList &fallbacks, ConfigBackend *backend)
    : ConfigPrivate()
{
    QSet<QString> opened;
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < paths.size(); i++) {
            // Firstly we should open user-specific configs
            ConfigSource::Ptr source = ConfigSource::open(paths.at(i), j == 1, sources.isEmpty(), backend);
            if (source && !opened.contains(source->fileName)) {
                opened.insert(source->fileName);
                sources << source;
            }
        }
    }
    for (int i = 0; i < sources.size(); i++) {
        ConfigSource::Ptr source = sources.at(i);
        current()->atoms << source->data;
    }
    for (int i = 0; i < fallbacks.size(); ++i) {
        const QVariant value = fallbacks.at(i);
        auto fallback = ConfigAtom::fromVariant(value, true);

        if (fallback->isNull() || fallback->isValue())
            continue;

        current()->atoms << fallback;
    }
}

ConfigPrivate::~ConfigPrivate()
{
    if (!memoryGuard)
        sync();
}

void ConfigPrivate::sync()
{
	if (sources.isEmpty())
		return;

    ConfigSource::Ptr source = sources.value(0);
	if (source && source->dirty && !source->isAtLoop) {
		source->isAtLoop = true;
		source->dirty = false;
		QCoreApplication::postEvent(postConfigSaver(), new PostConfigSaveEvent(source), -2);
    }
}

QExplicitlySharedDataPointer<ConfigPrivate> ConfigPrivate::clone()
{
    QExplicitlySharedDataPointer<ConfigPrivate> result(new ConfigPrivate);

    *result->current() = *current();
    result->sources = sources;

    return result;
}

Config::Config(const QVariantList &list) : d_ptr(new ConfigPrivate)
{
    Q_D(Config);
    d->current()->atoms << ConfigAtom::fromVariant(list, false);
}

Config::Config(const QVariantMap &map) : d_ptr(new ConfigPrivate)
{
    Q_D(Config);
    d->current()->atoms << ConfigAtom::fromVariant(map, false);
}

Config::Config(const QString &path)
    : d_ptr(new ConfigPrivate(QStringList(path)))
{
}

Config::Config(const QString &path, ConfigBackend *backend)
    : d_ptr(new ConfigPrivate(QStringList(path), backend))
{
}

Config::Config(const QStringList &paths)
    : d_ptr(new ConfigPrivate(paths))
{
}

Config::Config(const QString &path, const QVariantList &fallbacks)
    : d_ptr(new ConfigPrivate(QStringList(path), fallbacks))
{
}

Config::Config(const QString &path, const QVariant &fallback)
    : d_ptr(new ConfigPrivate(QStringList(path), QVariantList() << fallback))
{
}

Config::Config(const QExplicitlySharedDataPointer<ConfigPrivate> &d) : d_ptr(d)
{
}

Config::Config(const Config &other) : d_ptr(other.d_ptr)
{
}

Config &Config::operator =(const Config &other)
{
	d_ptr = other.d_ptr;
	return *this;
}

Config::~Config()
{
}

Config Config::group(const QString &fullName)
{
	Q_D(Config);

    Q_ASSERT(!fullName.isEmpty());

    beginGroup(fullName);
    auto p = d->clone();
    p->memoryGuard = d_ptr;
    endGroup();

    return Config(p);
}

QStringList Config::childGroups() const
{
	Q_D(const Config);
	QStringList groups;

    d->current()->iterateMap([&groups] (const QString &name, const ConfigAtom::Ptr &atom) {
        if (atom->isMap() && !groups.contains(name))
            groups << name;
    });

	return groups;
}

QStringList Config::childKeys() const
{
	Q_D(const Config);
    QStringList keys;

    d->current()->iterateMap([&keys] (const QString &name, const ConfigAtom::Ptr &atom) {
        if (!atom->isMap() && !keys.contains(name))
            keys << name;
    });

    return keys;
}

bool Config::hasChildGroup(const QString &name) const
{
	Q_D(const Config);
    bool found = false;

    d->current()->iterateMap([&found, name] (const QString &keyName, const ConfigAtom::Ptr &atom) {
        if (atom->isMap() && keyName == name)
            found = true;
    });

    return found;
}

bool Config::hasChildKey(const QString &name) const
{
    Q_D(const Config);
    bool found = false;

    d->current()->iterateMap([&found, name] (const QString &keyName, const ConfigAtom::Ptr &atom) {
        if (!atom->isMap() && keyName == name)
            found = true;
    });

    return found;
}

static QStringList parseNames(const QString &fullName)
{
	QStringList names;
	int first = 0;
	do {
		int last = fullName.indexOf('/', first);
		QString name = fullName.mid(first, last != -1 ? last - first : last);
		first = last + 1;
		if (!name.isEmpty())
			names << name;
	} while(first != 0);
	return names;
}

void Config::beginGroup(const QString &fullName)
{
	Q_D(Config);
	Q_ASSERT(!fullName.isEmpty());

    const QStringList names = parseNames(fullName);
    Q_ASSERT(!names.isEmpty());

    d->levels.prepend(d->current()->child(names)->convert(ConfigAtom::Map));
}

void Config::endGroup()
{
	Q_D(Config);
    Q_ASSERT(d->levels.size() > 1);
    d->levels.takeFirst();
}

void Config::remove(const QString &name)
{
    Q_D(Config);
    const ConfigAtom::Ptr atom = d->current()->atoms.value(0);
    if (atom->remove(name))
        d->sources.first()->makeDirty();
}

Config Config::arrayElement(int index)
{
    Q_D(Config);

    auto p = d->clone();
    p->memoryGuard = d_ptr;

    Config cfg(p);
    cfg.setArrayIndex(index);
    return cfg;
}

int Config::beginArray(const QString &name)
{
    Q_D(Config);
    Q_ASSERT(!name.isEmpty());

    const QStringList names = parseNames(name);
    d->levels.prepend(d->current()->child(names)->convert(ConfigAtom::List));

    return arraySize();
}

void Config::endArray()
{
	Q_D(Config);
	Q_ASSERT(d->levels.size() > 1);

    if (d->current()->arrayElement)
        d->levels.takeFirst();

    Q_ASSERT(d->levels.size() > 1);
    Q_ASSERT(!d->current()->arrayElement);
    d->levels.takeFirst();
}

int Config::arraySize() const
{
    Q_D(const Config);

    const ConfigLevel::Ptr level = d->current()->arrayElement ? d->levels.at(1) : d->current();

    int size = 0;
    for (ConfigAtom::Ptr atom : level->atoms) {
        if ((size = atom->arraySize()) > 0)
            break;
    }

    return size;
}

void Config::setArrayIndex(int index)
{
	Q_D(Config);

    if (d->current()->arrayElement)
        d->levels.takeFirst();

    const ConfigLevel::Ptr level = d->current();
    Q_ASSERT(level->atoms.first()->isList());

    const ConfigLevel::Ptr arrayElement = level->child(index)->convert(ConfigAtom::Map);
    arrayElement->arrayElement = true;
    d->levels.prepend(arrayElement);
}

void Config::remove(int index)
{
	Q_D(Config);

    if (d->current()->arrayElement)
        d->levels.takeFirst();

    ConfigAtom::Ptr atom = d->current()->atoms.value(0);
    if (atom->remove(index))
        d->sources.first()->makeDirty();
}

QVariant Config::rootValue(const QVariant &def, ValueFlags type) const
{
	Q_D(const Config);

    if (d->current()->atoms.isEmpty())
		return def;

    QVariant var = d->current()->atoms.first()->toVariant();
	if (type & Config::Crypted)
		return var.isNull() ? def : CryptoService::decrypt(var);
	else
		return var.isNull() ? def : var;
}

QVariant Config::value(const QString &key, const QVariant &def, ValueFlags type) const
{
	Q_D(const Config);

    if (d->current()->atoms.isEmpty())
		return def;

	QString name = key;
	int slashIndex = name.lastIndexOf('/');
	if (slashIndex != -1) {
		const_cast<Config*>(this)->beginGroup(name.mid(0, slashIndex));
		name = name.mid(slashIndex + 1);
	}

	const ConfigLevel::Ptr &level = d->current();

	QVariant var;
    QList<ConfigAtom::Ptr> &atoms = level->atoms;
	for (int i = 0; i < atoms.size(); i++) {
        ConfigAtom::Ptr atom = level->atoms.at(i);
        Q_ASSERT(atom->isMap());

        ConfigAtom::Ptr child = atom->child(name);
        if (child && !child->isNull())
            var = child->toVariant();

		if (!var.isNull())
			break;
	}

	if (slashIndex != -1)
		const_cast<Config*>(this)->endGroup();

	if (type & Config::Crypted)
		return var.isNull() ? def : CryptoService::decrypt(var);
	else
		return var.isNull() ? def : var;
}

void Config::setValue(const QString &key, const QVariant &value, ValueFlags type)
{
	Q_D(Config);
    if (d->current()->atoms.isEmpty())
		return;

	QString name = key;
	int slashIndex = name.lastIndexOf('/');
	if (slashIndex != -1) {
		beginGroup(name.mid(0, slashIndex));
		name = name.mid(slashIndex + 1);
	}

    QVariant var = (type & Config::Crypted) ? CryptoService::crypt(value) : value;

    ConfigAtom::Ptr atom = d->current()->atoms.first();
    Q_ASSERT(atom->isMap() && !atom->isReadOnly());
    if (atom->replace(name, ConfigAtom::fromVariant(var, false))) {
        if (!d->sources.isEmpty())
            d->sources.first()->makeDirty();
    }

	if (slashIndex != -1)
		endGroup();
}

void Config::sync()
{
	d_func()->sync();
}

void Config::registerType(int type, SaveOperator saveOp, LoadOperator loadOp)
{
	Q_UNUSED(type);
	Q_UNUSED(saveOp);
	Q_UNUSED(loadOp);
}

class ConfigBackendPrivate
{
public:
	mutable QByteArray extension;
};

ConfigBackend::ConfigBackend() : d_ptr(new ConfigBackendPrivate)
{
}

ConfigBackend::~ConfigBackend()
{
}

QByteArray ConfigBackend::name() const
{
	Q_D(const ConfigBackend);
	if(d->extension.isNull()) {
		d->extension = MetaObjectBuilder::info(metaObject(), "Extension");
		d->extension = d->extension.toLower();
	}
	return d->extension;
}

void ConfigBackend::virtual_hook(int id, void *data)
{
	Q_UNUSED(id);
    Q_UNUSED(data);
}

}

