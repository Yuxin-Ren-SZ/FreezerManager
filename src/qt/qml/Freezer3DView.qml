import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick3D

Rectangle {
    id: root
    color: "#2c3e50"

    // Freezer data from C++ context
    property var freezerData: []

    RowLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 10

        // Sidebar: Freezer list
        Rectangle {
            Layout.preferredWidth: 220
            Layout.fillHeight: true
            color: "#34495e"
            radius: 6

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10

                Text {
                    text: "Freezers"
                    color: "#ecf0f1"
                    font.pixelSize: 18
                    font.bold: true
                }

                ListView {
                    id: freezerList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    model: freezerData
                    clip: true

                    delegate: Rectangle {
                        width: freezerList.width
                        height: 60
                        color: mouseArea.containsMouse ? "#3d566e" : "transparent"
                        radius: 4

                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 10

                            Text {
                                text: modelData.name || "Unknown"
                                color: "#ecf0f1"
                                font.pixelSize: 13
                                font.bold: true
                            }
                            Text {
                                text: (modelData.temp !== undefined ? modelData.temp + "°C" : "")
                                color: modelData.temp < -150 ? "#3498db" : "#e74c3c"
                                font.pixelSize: 11
                            }
                        }

                        MouseArea {
                            id: mouseArea
                            anchors.fill: parent
                            hoverEnabled: true
                        }
                    }
                }
            }
        }

        // Main 3D viewport
        View3D {
            id: view3D
            Layout.fillWidth: true
            Layout.fillHeight: true

            environment: SceneEnvironment {
                clearColor: "#1a252f"
                backgroundMode: SceneEnvironment.Color
            }

            // Camera
            PerspectiveCamera {
                id: camera
                position: Qt.vector3d(0, 200, 400)
                eulerRotation: Qt.vector3d(-20, 0, 0)
            }

            // Directional light
            DirectionalLight {
                eulerRotation: Qt.vector3d(-30, -45, 0)
            }

            // Ambient light (PointLight as fallback for Qt 6.8+)
            PointLight {
                color: "#606060"
                position: Qt.vector3d(0, 300, 200)
            }

            // --- Freezer body ---
            Node {
                id: freezerGroup

                // Main body (white box)
                Model {
                    source: "#Cube"
                    position: Qt.vector3d(0, 100, 0)
                    scale: Qt.vector3d(1.8, 1.0, 1.2)
                    materials: [
                        PrincipledMaterial {
                            baseColor: "#ecf0f1"
                            metalness: 0.1
                            roughness: 0.3
                        }
                    ]
                }

                // Door (slightly offset, darker)
                Model {
                    source: "#Cube"
                    position: Qt.vector3d(0, 100, 72)
                    scale: Qt.vector3d(1.75, 0.95, 0.05)
                    materials: [
                        PrincipledMaterial {
                            baseColor: "#bdc3c7"
                            metalness: 0.3
                            roughness: 0.2
                        }
                    ]
                }

                // Handle
                Model {
                    source: "#Cylinder"
                    position: Qt.vector3d(90, 120, 80)
                    scale: Qt.vector3d(0.1, 0.3, 0.1)
                    eulerRotation: Qt.vector3d(0, 0, 90)
                    materials: [
                        PrincipledMaterial {
                            baseColor: "#7f8c8d"
                            metalness: 0.8
                            roughness: 0.2
                        }
                    ]
                }

                // Shelves (5 horizontal planes inside)
                Repeater3D {
                    model: 5
                    delegate: Model {
                        source: "#Cube"
                        position: Qt.vector3d(0, 50 + index * 35, 0)
                        scale: Qt.vector3d(1.7, 0.02, 1.1)
                        materials: [
                            PrincipledMaterial {
                                baseColor: "#95a5a6"
                                metalness: 0.2
                                roughness: 0.5
                            }
                        ]
                    }
                }

                // Sample boxes on shelves (colored cubes)
                Repeater3D {
                    model: 15
                    delegate: Model {
                        source: "#Cube"
                        property int shelfIdx: Math.floor(index / 3)
                        property int colIdx: index % 3
                        position: Qt.vector3d(
                            -60 + colIdx * 60,
                            65 + shelfIdx * 35,
                            20
                        )
                        scale: Qt.vector3d(0.3, 0.12, 0.3)
                        materials: [
                            PrincipledMaterial {
                                baseColor: {
                                    var colors = ["#3498db", "#2ecc71", "#e74c3c",
                                                 "#f39c12", "#9b59b6"];
                                    return colors[index % 5];
                                }
                                metalness: 0.0
                                roughness: 0.6
                            }
                        ]
                    }
                }

                // Temperature display
                Text {
                    text: "−80°C"
                    color: "#3498db"
                    font.pixelSize: 24
                    font.bold: true
                    // Position approximately at bottom of door
                    x: 300
                    y: 350
                }
            }
        }
    }
}
