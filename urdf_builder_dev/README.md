# STL Reorienter  
STLの軸や中心を再設定できるpythonです。  

<img width="400" src="https://github.com/user-attachments/assets/0732bd7a-2c48-4147-bbb5-7af2c1a22864">
  
## 機能  
・読み込んだSTLファイルのオブジェクトを0,0,0を原点として表示する。  
・STLの0,0,0を再設定できる。  
・座標軸を再設定できる。  
（画面に見えている状態の上をzプラス、右をyプラスとして保存できる。）  
  
## ライブラリ等のバージョン  
- python Version: 3.11
- numpy Version: 2.1.1
- PyQt5 Version: 5.15.11
- pvtk　Version: 9.3.1
にて動作を確認しました。  
  
## 実行方法  
```python stl_reorienter.py```  
  
## 操作方法  
as,ws,qeキー：座標軸を上下、左右、ロールに90度ずつ回転させる  
rキー：カメラをリセットする  
tキー：オブジェクト表示の透過の切り替え  
カーソルキー；新しい原点となるポイントの移動（画面に対して上下左右に10mmずつ移動）  
シフト＋カーソル：1ミリずつ移動  
コマンド(Ctrl)＋カーソル：0.1ミリずつ移動  
Ctrl + c ：終了  