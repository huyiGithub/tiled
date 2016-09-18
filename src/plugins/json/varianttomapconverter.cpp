/*
 * JSON Tiled Plugin
 * Copyright 2011, Porfírio José Pereira Ribeiro <porfirioribeiro@gmail.com>
 * Copyright 2011, Thorbjørn Lindeijer <thorbjorn@lindeijer.nl>
 *
 * This file is part of Tiled.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "varianttomapconverter.h"

#include "imagelayer.h"
#include "map.h"
#include "mapobject.h"
#include "objectgroup.h"
#include "properties.h"
#include "tile.h"
#include "tilelayer.h"
#include "tileset.h"

#include <QScopedPointer>

using namespace Tiled;
using namespace Json;

static QString resolvePath(const QDir &dir, const QVariant &variant)
{
    QString fileName = variant.toString();
    if (QDir::isRelativePath(fileName))
        fileName = QDir::cleanPath(dir.absoluteFilePath(fileName));
    return fileName;
}

Map *VariantToMapConverter::toMap(const QVariant &variant,
                                  const QDir &mapDir)
{
    mGidMapper.clear();
    mMapDir = mapDir;

    const QVariantMap variantMap = variant.toMap();
    const QString orientationString = variantMap["orientation"].toString();

    Map::Orientation orientation = orientationFromString(orientationString);

    if (orientation == Map::Unknown) {
        mError = tr("Unsupported map orientation: \"%1\"")
                .arg(orientationString);
        return 0;
    }

    const QString staggerAxisString = variantMap["staggeraxis"].toString();
    Map::StaggerAxis staggerAxis = staggerAxisFromString(staggerAxisString);

    const QString staggerIndexString = variantMap["staggerindex"].toString();
    Map::StaggerIndex staggerIndex = staggerIndexFromString(staggerIndexString);

    const QString renderOrderString = variantMap["renderorder"].toString();
    Map::RenderOrder renderOrder = renderOrderFromString(renderOrderString);

    const int nextObjectId = variantMap["nextobjectid"].toString().toInt();

    QScopedPointer<Map> map(new Map(orientation,
                            variantMap["width"].toInt(),
                            variantMap["height"].toInt(),
                            variantMap["tilewidth"].toInt(),
                            variantMap["tileheight"].toInt()));
    map->setHexSideLength(variantMap["hexsidelength"].toInt());
    map->setStaggerAxis(staggerAxis);
    map->setStaggerIndex(staggerIndex);
    map->setRenderOrder(renderOrder);

    // RTB
    toRTBMap(variantMap, map->rtbMap());

    if (nextObjectId)
        map->setNextObjectId(nextObjectId);

    mMap = map.data();
    map->setProperties(toProperties(variantMap["properties"]));

    const QString bgColor = variantMap["backgroundcolor"].toString();
    if (!bgColor.isEmpty() && QColor::isValidColor(bgColor))
        map->setBackgroundColor(QColor(bgColor));

    foreach (const QVariant &tilesetVariant, variantMap["tilesets"].toList()) {
        SharedTileset tileset = toTileset(tilesetVariant);
        if (!tileset)
            return 0;

        map->addTileset(tileset);
    }

    foreach (const QVariant &layerVariant, variantMap["layers"].toList()) {
        Layer *layer = toLayer(layerVariant);
        if (!layer)
            return 0;

        map->addLayer(layer);
    }

    return map.take();
}

Properties VariantToMapConverter::toProperties(const QVariant &variant)
{
    const QVariantMap variantMap = variant.toMap();

    Properties properties;

    QVariantMap::const_iterator it = variantMap.constBegin();
    QVariantMap::const_iterator it_end = variantMap.constEnd();
    for (; it != it_end; ++it)
        properties[it.key()] = it.value().toString();

    return properties;
}

SharedTileset VariantToMapConverter::toTileset(const QVariant &variant)
{
    const QVariantMap variantMap = variant.toMap();

    const int firstGid = variantMap["firstgid"].toInt();
    const QString name = variantMap["name"].toString();
    const int tileWidth = variantMap["tilewidth"].toInt();
    const int tileHeight = variantMap["tileheight"].toInt();
    const int spacing = variantMap["spacing"].toInt();
    const int margin = variantMap["margin"].toInt();
    const QVariantMap tileOffset = variantMap["tileoffset"].toMap();
    const int tileOffsetX = tileOffset["x"].toInt();
    const int tileOffsetY = tileOffset["y"].toInt();

    if (tileWidth <= 0 || tileHeight <= 0 || firstGid == 0) {
        mError = tr("Invalid tileset parameters for tileset '%1'").arg(name);
        return SharedTileset();
    }

    SharedTileset tileset(Tileset::create(name,
                                          tileWidth, tileHeight,
                                          spacing, margin));

    tileset->setTileOffset(QPoint(tileOffsetX, tileOffsetY));

    const QString trans = variantMap["transparentcolor"].toString();
    if (!trans.isEmpty() && QColor::isValidColor(trans))
        tileset->setTransparentColor(QColor(trans));

    QVariant imageVariant = variantMap["image"];

    if (!imageVariant.isNull()) {
        // RTB Tileset
        //QString imagePath = resolvePath(mMapDir, imageVariant);
        QString imagePath = QString::fromStdString(":/rtb_resources/tileset/Floor.png");
        if (!tileset->loadFromImage(imagePath)) {
            mError = tr("Error loading tileset image:\n'%1'").arg(imagePath);
            return SharedTileset();
        }
    }

    tileset->setProperties(toProperties(variantMap["properties"]));

    // Read terrains
    QVariantList terrainsVariantList = variantMap["terrains"].toList();
    for (int i = 0; i < terrainsVariantList.count(); ++i) {
        QVariantMap terrainMap = terrainsVariantList[i].toMap();
        tileset->addTerrain(terrainMap["name"].toString(),
                            terrainMap["tile"].toInt());
    }

    // Read tile terrain and external image information
    const QVariantMap tilesVariantMap = variantMap["tiles"].toMap();
    QVariantMap::const_iterator it = tilesVariantMap.constBegin();
    for (; it != tilesVariantMap.end(); ++it) {
        bool ok;
        const int tileIndex = it.key().toInt();
        if (tileIndex < 0) {
            mError = tr("Tileset tile index negative:\n'%1'").arg(tileIndex);
        }

        if (tileIndex >= tileset->tileCount()) {
            // Extend the tileset to fit the tile
            if (tileIndex >= tilesVariantMap.count()) {
                // If tiles are  defined this way, there should be an entry
                // for each tile.
                // Limit the index to number of entries to prevent running out
                // of memory on malicious input.
                mError = tr("Tileset tile index too high:\n'%1'").arg(tileIndex);
                return SharedTileset();
            }
            for (int i = tileset->tileCount(); i <= tileIndex; i++)
                tileset->addTile(QPixmap());
        }

        Tile *tile = tileset->tileAt(tileIndex);
        if (tile) {
            const QVariantMap tileVar = it.value().toMap();
            QList<QVariant> terrains = tileVar["terrain"].toList();
            if (terrains.count() == 4) {
                for (int i = 0; i < 4; ++i) {
                    int terrainId = terrains.at(i).toInt(&ok);
                    if (ok && terrainId >= 0 && terrainId < tileset->terrainCount())
                        tile->setCornerTerrainId(i, terrainId);
                }
            }
            float terrainProbability = tileVar["probability"].toFloat(&ok);
            if (ok)
                tile->setTerrainProbability(terrainProbability);
            imageVariant = tileVar["image"];
            if (!imageVariant.isNull()) {
                QString imagePath = resolvePath(mMapDir, imageVariant);
                tileset->setTileImage(tileIndex, QPixmap(imagePath), imagePath);
            }
            QVariantMap objectGroupVariant = tileVar["objectgroup"].toMap();
            if (!objectGroupVariant.isEmpty())
                tile->setObjectGroup(toObjectGroup(objectGroupVariant));

            QVariantList frameList = tileVar["animation"].toList();
            if (!frameList.isEmpty()) {
                QVector<Frame> frames(frameList.size());
                for (int i = frameList.size() - 1; i >= 0; --i) {
                    const QVariantMap frameVariantMap = frameList[i].toMap();
                    Frame &frame = frames[i];
                    frame.tileId = frameVariantMap["tileid"].toInt();
                    frame.duration = frameVariantMap["duration"].toInt();
                }
                tile->setFrames(frames);
            }
        }
    }

    // Read tile properties
    QVariantMap propertiesVariantMap = variantMap["tileproperties"].toMap();
    for (it = propertiesVariantMap.constBegin(); it != propertiesVariantMap.constEnd(); ++it) {
        const int tileIndex = it.key().toInt();
        const QVariant propertiesVar = it.value();
        if (tileIndex >= 0 && tileIndex < tileset->tileCount()) {
            const Properties properties = toProperties(propertiesVar);
            tileset->tileAt(tileIndex)->setProperties(properties);
        }
    }

    mGidMapper.insert(firstGid, tileset.data());
    return tileset;
}

Layer *VariantToMapConverter::toLayer(const QVariant &variant)
{
    const QVariantMap variantMap = variant.toMap();
    Layer *layer = 0;

    if (variantMap["type"] == "tilelayer")
        layer = toTileLayer(variantMap);
    else if (variantMap["type"] == "objectgroup")
        layer = toObjectGroup(variantMap);
    else if (variantMap["type"] == "imagelayer")
        layer = toImageLayer(variantMap);

    if (layer)
        layer->setProperties(toProperties(variantMap["properties"]));

    return layer;
}

TileLayer *VariantToMapConverter::toTileLayer(const QVariantMap &variantMap)
{
    const QString name = variantMap["name"].toString();
    const int width = variantMap["width"].toInt();
    const int height = variantMap["height"].toInt();
    const QVariantList dataVariantList = variantMap["data"].toList();

    if (dataVariantList.size() != width * height) {
        mError = tr("Corrupt layer data for layer '%1'").arg(name);
        return 0;
    }

    typedef QScopedPointer<TileLayer> TileLayerPtr;
    TileLayerPtr tileLayer(new TileLayer(name,
                                         variantMap["x"].toInt(),
                                         variantMap["y"].toInt(),
                                         width, height));

    const qreal opacity = variantMap["opacity"].toReal();
    const bool visible = variantMap["visible"].toBool();

    tileLayer->setOpacity(opacity);
    tileLayer->setVisible(visible);

    int x = 0;
    int y = 0;
    bool ok;

    foreach (const QVariant &gidVariant, dataVariantList) {
        const unsigned gid = gidVariant.toUInt(&ok);
        if (!ok) {
            mError = tr("Unable to parse tile at (%1,%2) on layer '%3'")
                    .arg(x).arg(y).arg(tileLayer->name());
            return 0;
        }

        const Cell cell = mGidMapper.gidToCell(gid, ok);

        tileLayer->setCell(x, y, cell);

        x++;
        if (x >= tileLayer->width()) {
            x = 0;
            y++;
        }
    }

    return tileLayer.take();
}

ObjectGroup *VariantToMapConverter::toObjectGroup(const QVariantMap &variantMap)
{
    typedef QScopedPointer<ObjectGroup> ObjectGroupPtr;
    ObjectGroupPtr objectGroup(new ObjectGroup(variantMap["name"].toString(),
                                               variantMap["x"].toInt(),
                                               variantMap["y"].toInt(),
                                               variantMap["width"].toInt(),
                                               variantMap["height"].toInt()));

    const qreal opacity = variantMap["opacity"].toReal();
    const bool visible = variantMap["visible"].toBool();

    objectGroup->setOpacity(opacity);
    objectGroup->setVisible(visible);

    objectGroup->setColor(variantMap.value("color").value<QColor>());

    const QString drawOrderString = variantMap.value("draworder").toString();
    if (!drawOrderString.isEmpty()) {
        objectGroup->setDrawOrder(drawOrderFromString(drawOrderString));
        if (objectGroup->drawOrder() == ObjectGroup::UnknownOrder) {
            mError = tr("Invalid draw order: %1").arg(drawOrderString);
            return 0;
        }
    }

    foreach (const QVariant &objectVariant, variantMap["objects"].toList()) {
        const QVariantMap objectVariantMap = objectVariant.toMap();

        const QString name = objectVariantMap["name"].toString();
        const QString type = objectVariantMap["type"].toString();
        const int id = objectVariantMap["id"].toString().toInt();
        const int gid = objectVariantMap["gid"].toInt();
        const qreal x = objectVariantMap["x"].toReal();
        const qreal y = objectVariantMap["y"].toReal();
        const qreal width = objectVariantMap["width"].toReal();
        const qreal height = objectVariantMap["height"].toReal();
        const qreal rotation = objectVariantMap["rotation"].toReal();

        const QPointF pos(x, y);
        const QSizeF size(width, height);

        MapObject *object = new MapObject(name, type, pos, size);
        object->setId(id);
        object->setRotation(rotation);

        if (gid) {
            bool ok;
            object->setCell(mGidMapper.gidToCell(gid, ok));

            if (!object->cell().isEmpty()) {
                const QSizeF &tileSize = object->cell().tile->size();
                if (width == 0)
                    object->setWidth(tileSize.width());
                if (height == 0)
                    object->setHeight(tileSize.height());
            }
        }

        if (objectVariantMap.contains("visible"))
            object->setVisible(objectVariantMap["visible"].toBool());

        object->setProperties(toProperties(objectVariantMap["properties"]));
        objectGroup->addObject(object);

        const QVariant polylineVariant = objectVariantMap["polyline"];
        const QVariant polygonVariant = objectVariantMap["polygon"];

        if (polygonVariant.isValid()) {
            object->setShape(MapObject::Polygon);
            object->setPolygon(toPolygon(polygonVariant));
        }
        if (polylineVariant.isValid()) {
            object->setShape(MapObject::Polyline);
            object->setPolygon(toPolygon(polylineVariant));
        }
        if (objectVariantMap.contains("ellipse"))
            object->setShape(MapObject::Ellipse);

        // RTB
        object->createRTBMapObject();
        toRTBMapObject(objectVariantMap, object->rtbMapObject());

    }

    return objectGroup.take();
}

ImageLayer *VariantToMapConverter::toImageLayer(const QVariantMap &variantMap)
{
    typedef QScopedPointer<ImageLayer> ImageLayerPtr;
    ImageLayerPtr imageLayer(new ImageLayer(variantMap["name"].toString(),
                                            variantMap["x"].toInt(),
                                            variantMap["y"].toInt(),
                                            variantMap["width"].toInt(),
                                            variantMap["height"].toInt()));

    const qreal opacity = variantMap["opacity"].toReal();
    const bool visible = variantMap["visible"].toBool();

    imageLayer->setOpacity(opacity);
    imageLayer->setVisible(visible);

    const QString trans = variantMap["transparentcolor"].toString();
    if (!trans.isEmpty() && QColor::isValidColor(trans))
        imageLayer->setTransparentColor(QColor(trans));

    QVariant imageVariant = variantMap["image"].toString();

    if (!imageVariant.isNull()) {
        QString imagePath = resolvePath(mMapDir, imageVariant);
        if (!imageLayer->loadFromImage(QImage(imagePath), imagePath)) {
            mError = tr("Error loading image:\n'%1'").arg(imagePath);
            return 0;
        }
    }

    return imageLayer.take();
}

QPolygonF VariantToMapConverter::toPolygon(const QVariant &variant) const
{
    QPolygonF polygon;
    foreach (const QVariant &pointVariant, variant.toList()) {
        const QVariantMap pointVariantMap = pointVariant.toMap();
        const qreal pointX = pointVariantMap["x"].toReal();
        const qreal pointY = pointVariantMap["y"].toReal();
        polygon.append(QPointF(pointX, pointY));
    }
    return polygon;
}

void VariantToMapConverter::toRTBMap(const QVariantMap &variantMap, RTBMap *rtbMap)
{
    rtbMap->setHasError(variantMap["haserror"].toInt());

    QString color = variantMap["customglowcolor"].toString();
    if (!color.isEmpty() && QColor::isValidColor(color))
        rtbMap->setCustomGlowColor(QColor(color));
    color = variantMap["custombackgroundcolor"].toString();
    if (!color.isEmpty() && QColor::isValidColor(color))
        rtbMap->setCustomBackgroundColor(QColor(color));

    rtbMap->setLevelBrightness(variantMap["levelbrightness"].toDouble());
    rtbMap->setCloudDensity(variantMap["clouddensity"].toDouble());
    rtbMap->setCloudVelocity(variantMap["cloudvelocity"].toDouble());
    rtbMap->setCloudAlpha(variantMap["cloudalpha"].toDouble());
    rtbMap->setSnowDensity(variantMap["snowdensity"].toDouble());
    rtbMap->setSnowVelocity(variantMap["snowvelocity"].toDouble());
    rtbMap->setSnowRisingVelocity(variantMap["snowrisingvelocity"].toDouble());
    rtbMap->setCameraGrain(variantMap["cameragrain"].toDouble());
    rtbMap->setCameraContrast(variantMap["cameracontrast"].toDouble());
    rtbMap->setCameraSaturation(variantMap["camerasaturation"].toDouble());
    rtbMap->setCameraGlow(variantMap["cameraglow"].toDouble());
    rtbMap->setHasWall(variantMap["haswalls"].toInt());
    rtbMap->setLevelName(variantMap["levelname"].toString());
    rtbMap->setLevelDescription(variantMap["leveldescription"].toString());
    rtbMap->setBackgroundColorScheme(variantMap["backgroundcolorscheme"].toInt());
    rtbMap->setGlowColorScheme(variantMap["glowcolorscheme"].toInt());
    rtbMap->setChapter(variantMap["chapter"].toInt());
    rtbMap->setHasStarfield(variantMap["hasstarfield"].toInt());
    rtbMap->setDifficulty(variantMap["difficulty"].toInt());
    rtbMap->setPlayStyle(variantMap["playstyle"].toInt());
    rtbMap->setWorkShopId(variantMap["workshopid"].toInt());
    rtbMap->setPreviewImagePath(variantMap["previewimagepath"].toString());

}

void VariantToMapConverter::toRTBMapObject(const QVariantMap &variantMap, Tiled::RTBMapObject *rtbMapObject)
{
    switch (rtbMapObject->objectType()) {
    case RTBMapObject::CustomFloorTrap:
    {
        RTBCustomFloorTrap *mapObject = static_cast<RTBCustomFloorTrap*>(rtbMapObject);
        mapObject->setIntervalSpeed(variantMap["intervalspeed"].toInt());
        mapObject->setIntervalOffset(variantMap["intervaloffset"].toInt());

        break;
    }
    case RTBMapObject::MovingFloorTrapSpawner:
    {
        RTBMovingFloorTrapSpawner *mapObject = static_cast<RTBMovingFloorTrapSpawner*>(rtbMapObject);
        mapObject->setSpawnAmount(variantMap["spawnamount"].toInt());
        mapObject->setIntervalSpeed(variantMap["intervalspeed"].toInt());
        mapObject->setRandomizeStart(variantMap["randomizestart"].toInt());

        break;
    }
    case RTBMapObject::Button:
    {
        RTBButtonObject *mapObject = static_cast<RTBButtonObject*>(rtbMapObject);
        mapObject->setBeatsActive(variantMap["beatsactive"].toInt());
        mapObject->setLaserBeamTargets(variantMap["laserbeamtargets"].toString());

        break;
    }
    case RTBMapObject::LaserBeam:
    {
        RTBLaserBeam *mapObject = static_cast<RTBLaserBeam*>(rtbMapObject);
        mapObject->setBeamType(variantMap["beamtype"].toInt());
        mapObject->setActivatedOnStart(variantMap["activatedonstart"].toInt());
        mapObject->setDirectionDegrees(variantMap["directiondegrees"].toInt());
        mapObject->setTargetDirectionDegrees(variantMap["targetdirectiondegrees"].toInt());
        mapObject->setIntervalOffset(variantMap["intervaloffset"].toInt());
        mapObject->setIntervalSpeed(variantMap["intervalspeed"].toInt());

        break;
    }
    case RTBMapObject::ProjectileTurret:
    {
        RTBProjectileTurret *mapObject = static_cast<RTBProjectileTurret*>(rtbMapObject);
        mapObject->setIntervalSpeed(variantMap["intervalspeed"].toInt());
        mapObject->setIntervalOffset(variantMap["intervaloffset"].toInt());
        mapObject->setProjectileSpeed(variantMap["projectilespeed"].toInt());
        mapObject->setShotDirection(variantMap["shotdirection"].toInt());

        break;
    }
    case RTBMapObject::Teleporter:
    {
        RTBTeleporter *mapObject = static_cast<RTBTeleporter*>(rtbMapObject);
        QString target = variantMap["teleportertarget"].toString();
        if(target != QLatin1String("0"))
            mapObject->setTeleporterTarget(target);
        else
            mapObject->setTeleporterTarget(QLatin1String(""));

        break;
    }
    case RTBMapObject::Target:
    {
        //RTBTeleporterTarget *mapObject = static_cast<RTBTeleporterTarget*>(rtbMapObject);
        break;
    }
    case RTBMapObject::FloorText:
    {
        RTBFloorText *mapObject = static_cast<RTBFloorText*>(rtbMapObject);
        mapObject->setText(variantMap["text"].toString());
        mapObject->setMaxCharacters(variantMap["maxcharacters"].toInt());
        int width = variantMap["triggerzonewidth"].toInt();
        int height = variantMap["triggerzoneheight"].toInt();
        mapObject->setTriggerZoneSize(QSizeF(width, height));
        mapObject->setUseTrigger(variantMap["usetrigger"].toInt());
        mapObject->setScale(variantMap["scale"].toDouble());
        mapObject->setOffsetX(variantMap["offsetx"].toDouble());
        mapObject->setOffsetY(variantMap["offsety"].toDouble());

        break;
    }
    case RTBMapObject::CameraTrigger:
    {
        RTBCameraTrigger *mapObject = static_cast<RTBCameraTrigger*>(rtbMapObject);
        QString target = variantMap["cameratarget"].toString();
        if(target != QLatin1String("0"))
            mapObject->setTarget(target);
        else
            mapObject->setTarget(QLatin1String(""));

        int width = variantMap["cameratriggerzonewidth"].toInt();
        int height = variantMap["cameratriggerzoneheight"].toInt();
        mapObject->setTriggerZoneSize(QSizeF(width, height));
        mapObject->setCameraHeight(variantMap["cameraheight"].toInt());
        mapObject->setCameraAngle(variantMap["cameraangle"].toInt());

        break;
    }
    case RTBMapObject::StartLocation:
    {
        //RTBStartLocation *mapObject = static_cast<RTBStartLocation*>(rtbMapObject);
        break;
    }
    case RTBMapObject::FinishHole:
    {
        //RTBFinishHole *mapObject = static_cast<RTBFinishHole*>(rtbMapObject);
        break;
    }

    case RTBMapObject::NPCBallSpawner:
    {
        RTBNPCBallSpawner *mapObject = static_cast<RTBNPCBallSpawner*>(rtbMapObject);
        mapObject->setSpawnClass(variantMap["spawnclass"].toInt());
        mapObject->setSize(variantMap["size"].toInt());
        mapObject->setIntervalOffset(variantMap["intervaloffset"].toInt());
        mapObject->setSpawnFrequency(variantMap["spawnfrequency"].toInt());
        mapObject->setSpeed(variantMap["speed"].toInt());
        mapObject->setDirection(variantMap["direction"].toInt());
        break;
    }
    default:

        return;
    }
}