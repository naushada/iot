import { Component, OnInit } from '@angular/core';
import { FormBuilder, FormGroup } from '@angular/forms';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { SessionService } from '../../../common/session.service';
import { ToastService } from '../../../common/toast.service';
import { WifiNetwork } from '../../../common/app-globals';

@Component({
  selector: 'app-wifi-config',
  templateUrl: './wifi-config.component.html',
  styleUrls: ['./wifi-config.component.scss']
})
export class WifiConfigComponent implements OnInit {
  form: FormGroup;
  networks: WifiNetwork[] = [];
  loading = true; saving = false; msg = '';

    get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private http: HttpsvcService, fb: FormBuilder, private session: SessionService, private toast: ToastService) {
    this.form = fb.group({
      iface:            ['wlan0'],
      wpa_path:         ['/usr/sbin/wpa_supplicant'],
      ctrl_dir:         ['/run/wpa_supplicant'],
      scan_interval:    [60],
      scan_max_results: [20],
      dhcp_client:      ['auto'],
      networks_json:    ['[]'],
    });
  }

  ngOnInit(): void {
    this.http.dbGet([
      'wifi.iface', 'wifi.wpa.path', 'wifi.ctrl.dir',
      'wifi.scan.interval.sec', 'wifi.scan.max.results',
      'wifi.dhcp.client', 'wifi.networks'
    ]).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const d = r.data as Record<string, unknown>;
          this.form.patchValue({
            iface: d['wifi.iface'] || 'wlan0',
            wpa_path: d['wifi.wpa.path'] || '/usr/sbin/wpa_supplicant',
            ctrl_dir: d['wifi.ctrl.dir'] || '/run/wpa_supplicant',
            scan_interval: d['wifi.scan.interval.sec'] || 60,
            scan_max_results: d['wifi.scan.max.results'] || 20,
            dhcp_client: d['wifi.dhcp.client'] || 'auto',
            networks_json: d['wifi.networks'] || '[]',
          });
          try { this.networks = JSON.parse(this.form.get('networks_json')!.value || '[]'); }
          catch { this.networks = []; }
        }
        this.loading = false;
      },
      error: () => { this.loading = false; }
    });
  }

  addNetwork(): void {
    this.networks.push({ ssid: '', psk: '', priority: 0, key_mgmt: 'WPA2-PSK' });
  }

  removeNetwork(i: number): void { this.networks.splice(i, 1); }

  save(): void {
    this.saving = true; this.msg = '';
    this.form.patchValue({ networks_json: JSON.stringify(this.networks) });
    const v = this.form.value;
    this.http.dbSet([
      { key: 'wifi.iface',               value: v.iface },
      { key: 'wifi.wpa.path',            value: v.wpa_path },
      { key: 'wifi.ctrl.dir',            value: v.ctrl_dir },
      { key: 'wifi.scan.interval.sec',   value: v.scan_interval },
      { key: 'wifi.scan.max.results',    value: v.scan_max_results },
      { key: 'wifi.dhcp.client',         value: v.dhcp_client },
      { key: 'wifi.networks',            value: v.networks_json },
    ]).subscribe({
      next: (r) => { this.saving = false; if(r.ok) this.toast.success('WiFi config saved'); else this.toast.error(r.err||'Save failed'); },
      error: () => { this.saving = false; this.toast.error('Save failed'); }
    });
  }
}
