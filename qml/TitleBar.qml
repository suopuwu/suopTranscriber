import QtQuick
import QtQuick.Controls
import SuopTranscriber

Item {
    id: titleBar

    property var window
    property string title: ""
    signal settingsRequested()

    implicitHeight: 36

    Rectangle {
        anchors.fill: parent
        color: "#1a1a1a"
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: if (tapCount === 2) titleBar.window.visibility = titleBar.window.visibility === Window.Maximized ? Window.Windowed : Window.Maximized
        gesturePolicy: TapHandler.DragThreshold
    }

    DragHandler {
        target: null
        onActiveChanged: if (active) titleBar.window.startSystemMove()
    }

    Item {
        id: settingsButton
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: 24
        height: 24

        HoverHandler {
            id: settingsHover
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            onTapped: titleBar.settingsRequested()
        }
        HoverBorder {
            anchors.fill: parent
            active: settingsHover.hovered
        }
        Text {
            anchors.centerIn: parent
            text: "⚙"
            color: "white"
            font.pixelSize: 14
        }
    }

    Text {
        anchors.left: settingsButton.right
        anchors.leftMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        text: titleBar.title
        color: "white"
    }

    Row {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        spacing: 0

        component TitleBarButton: Item {
            id: tbButton
            property string glyph: ""
            signal clicked()
            width: 46
            height: parent.height

            HoverHandler {
                id: hover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                onTapped: tbButton.clicked()
            }
            Rectangle {
                anchors.fill: parent
                color: hover.hovered ? "#3a3a3a" : "transparent"
            }
            HoverBorder {
                anchors.fill: parent
                anchors.margins: 4
                active: hover.hovered
            }
            Text {
                anchors.centerIn: parent
                text: tbButton.glyph
                color: "white"
            }
        }

        TitleBarButton {
            glyph: "−"
            onClicked: titleBar.window.showMinimized()
        }
        TitleBarButton {
            glyph: "□"
            onClicked: titleBar.window.visibility = titleBar.window.visibility === Window.Maximized ? Window.Windowed : Window.Maximized
        }
        TitleBarButton {
            glyph: "×"
            onClicked: titleBar.window.close()
        }
    }
}
