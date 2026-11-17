import SwiftUI
import CoreBluetooth
import CoreLocation

let service = CBUUID(string:"6e400001-b5a3-f393-e0a9-e50e24dcca9e")
let ack = CBUUID(string:"6e400002-b5a3-f393-e0a9-e50e24dcca9e")
let alert = CBUUID(string:"6e400003-b5a3-f393-e0a9-e50e24dcca9e")

final class Panel: NSObject, ObservableObject, CBCentralManagerDelegate, CBPeripheralDelegate, CLLocationManagerDelegate {
    @Published var active = false
    @Published var events = [String]()
    private lazy var central = CBCentralManager(delegate:self, queue:nil)
    private let location = CLLocationManager()
    private var peripheral: CBPeripheral?
    private var output: CBCharacteristic?
    private var pending: Date?
    private var writing = false
    private var replied = false

    func log(_ text: String) {
        events.insert(Date().formatted(date:.omitted,time:.standard) + "  " + text, at:0)
        events = Array(events.prefix(30))
    }
    func toggle() {
        active.toggle()
        if active {
            location.delegate = self
            location.requestWhenInUseAuthorization()
            location.startUpdatingLocation()
            if central.state == .poweredOn { seek() }
            log("Recepção ligada")
        } else {
            central.stopScan()
            if let p = peripheral { central.cancelPeripheralConnection(p) }
            location.stopUpdatingLocation()
            pending = nil
            log("Recepção pausada para testar o GPS")
        }
    }
    func seek() {
        guard active, central.state == .poweredOn else { return }
        if let key = UserDefaults.standard.string(forKey:"board"), let id = UUID(uuidString:key),
           let p = central.retrievePeripherals(withIdentifiers:[id]).first {
            connect(p)
        } else {
            central.scanForPeripherals(withServices:[service])
            log("Procurando a placa")
        }
    }
    func connect(_ p: CBPeripheral) {
        peripheral = p
        p.delegate = self
        central.connect(p)
        log("Aguardando a placa anunciar")
    }
    func centralManagerDidUpdateState(_ manager:CBCentralManager) {
        if manager.state == .poweredOn { seek() }
        else { log("Bluetooth indisponível ou aguardando autorização") }
    }
    func centralManager(_ c:CBCentralManager, didDiscover p:CBPeripheral, advertisementData:[String:Any], rssi:NSNumber) {
        guard active else { return }
        c.stopScan()
        connect(p)
    }
    func centralManager(_ c:CBCentralManager, didConnect p:CBPeripheral) {
        guard active else { c.cancelPeripheralConnection(p); return }
        UserDefaults.standard.set(p.identifier.uuidString,forKey:"board")
        pending = nil
        output = nil
        writing = false
        replied = false
        log("Bluetooth conectado")
        p.discoverServices([service])
    }
    func centralManager(_ c:CBCentralManager, didDisconnectPeripheral p:CBPeripheral, error:Error?) {
        pending = nil
        output = nil
        seek()
    }
    func centralManager(_ c:CBCentralManager, didFailToConnect p:CBPeripheral, error:Error?) {
        if active { c.scanForPeripherals(withServices:[service]); log("Buscando novamente") }
    }
    func peripheral(_ p:CBPeripheral, didDiscoverServices error:Error?) {
        if let s = p.services?.first(where:{$0.uuid == service}) { p.discoverCharacteristics([ack,alert],for:s) }
    }
    func peripheral(_ p:CBPeripheral, didDiscoverCharacteristicsFor s:CBService, error:Error?) {
        output = s.characteristics?.first(where:{$0.uuid == ack})
        if let input = s.characteristics?.first(where:{$0.uuid == alert}) { p.setNotifyValue(true,for:input) }
    }
    func peripheral(_ p:CBPeripheral, didUpdateValueFor ch:CBCharacteristic, error:Error?) {
        guard active, !replied, !writing, ch.uuid == alert, ch.value == Data("ALERT".utf8) else { return }
        if pending == nil { pending = Date(); log("A placa pediu a localização") }
        respond()
    }
    func locationManager(_ manager:CLLocationManager,didUpdateLocations locations:[CLLocation]) { respond() }
    func locationManager(_ manager:CLLocationManager,didFailWithError error:Error) { log("Localização indisponível") }
    func respond() {
        guard active, !writing, !replied, let since = pending, Date().timeIntervalSince(since) < 4,
              let fix = location.location, abs(fix.timestamp.timeIntervalSinceNow) <= 60,
              fix.horizontalAccuracy >= 0, let p = peripheral, p.state == .connected, let ch = output else { return }
        let data = String(format:"%.6f;%.6f;%.0f",locale:Locale(identifier:"en_US_POSIX"),
                          fix.coordinate.latitude,fix.coordinate.longitude,fix.horizontalAccuracy)
        writing = true
        p.writeValue(Data(data.utf8),for:ch,type:.withResponse)
    }
    func peripheral(_ p:CBPeripheral,didWriteValueFor ch:CBCharacteristic,error:Error?) {
        writing = false
        replied = error == nil
        log(replied ? "Posição enviada à placa" : "Falha ao responder")
    }
}
@main struct Demo: App {
    @StateObject private var panel = Panel()
    var body: some Scene {
        WindowGroup("Botão de emergência") {
            VStack(alignment:.leading,spacing:14) {
                Text("Localização pelo Mac").font(.title)
                Button(panel.active ? "Pausar recepção" : "Ativar recepção") { panel.toggle() }
                ScrollView { Text(panel.events.joined(separator:"\n")).font(.system(.body,design:.monospaced)).frame(maxWidth:.infinity,alignment:.leading) }
            }.padding(22).frame(minWidth:490,minHeight:330)
        }
    }
}
