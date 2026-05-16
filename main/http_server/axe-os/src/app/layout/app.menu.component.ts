import { Component, OnInit } from '@angular/core';
import { Observable, shareReplay } from 'rxjs';
import { SystemApiService } from '../services/system.service';
import { LayoutService } from './service/app.layout.service';
import { SystemInfo as ISystemInfo } from 'src/app/generated/models';

@Component({
  selector: 'app-menu',
  templateUrl: './app.menu.component.html'
})
export class AppMenuComponent implements OnInit {
  public info$!: Observable<ISystemInfo>;

  model: any[] = [];

  constructor(public layoutService: LayoutService,
    private systemService: SystemApiService,
  ) {
    this.info$ = this.systemService.getInfo().pipe(shareReplay({ refCount: true, bufferSize: 1 }))
  }

  ngOnInit() {
    this.model = [
      {
        label: 'Menu',
        items: [
          { label: 'Dashboard', icon: 'pi pi-fw pi-home', routerLink: ['/'] },
          { label: 'Scoreboard', icon: 'pi pi-fw pi-trophy', routerLink: ['scoreboard'] },
          { label: 'Swarm', icon: 'pi pi-fw pi-sitemap', routerLink: ['swarm'] },
          { label: 'Logs', icon: 'pi pi-fw pi-list', routerLink: ['logs'] },
          { label: 'System', icon: 'pi pi-fw pi-wave-pulse', routerLink: ['system'] },
          { separator: true },

          { label: 'Network', icon: 'pi pi-fw pi-wifi', routerLink: ['network'] },
          { label: 'Theme', icon: 'pi pi-fw pi-palette', routerLink: ['design'] },
          { label: 'Settings', icon: 'pi pi-fw pi-cog', routerLink: ['settings'] },
          { separator: true },

          { label: 'Whitepaper', icon: 'pi pi-fw pi-bitcoin', command: () => window.open('https://norugcoin.punkyshungry.com/whitepaper.html', '_blank') },
        ]
      }
    ];
  }
}
