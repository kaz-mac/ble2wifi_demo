<?php
//
// M5Stackから送信されたデータをCSVに格納する
//

// 設定
$SAVE_DIR = "/path/to/your/dir";	// 保存先ディレクトリ（パーミッションの変更を忘れずに）
$PREFIX = "xiao";	// プレフィックス

// 引数を取り込み
$text = file_get_contents("php://input");
$json = json_decode($text, true);
if (!is_numeric($json['count'] ?? null) || !is_array($json['data'] ?? null)) {
	header("HTTP/1.1 400 Bad Request");
	exit;
}

// ファイルに保存
$update = 0;
if (count($json['data']) > 0) {
	foreach ($json['data'] as $dd) {
		if (!is_numeric($dd['id'] ?? null)) continue;
		$seq_path = sprintf("%s/%s_%d.seq", $SAVE_DIR, $PREFIX, $dd['id']);
		$csv_path = sprintf("%s/%s_%d.csv", $SAVE_DIR, $PREFIX, $dd['id']);
		$allcsv_path = sprintf("%s/%s_all.csv", $SAVE_DIR, $PREFIX);
		$last_seq = (file_exists($seq_path)) ? file_get_contents($seq_path) : -1;
		if ($dd['seq'] != $last_seq) {
			$csv = [
				date("Y-m-d H:i:s"),
				$dd['id'],
				$dd['volt'],
				$dd['temp'],
				$dd['rssi'],
				$dd['seq'],
			];
			$str = join(",",$csv)."\n";
			error_log($str, 3, $csv_path);
			error_log($str, 3, $allcsv_path);
			file_put_contents($seq_path, $dd['seq']);
			$update ++;
		}
	}
}

// 終了
$json = [ 'update'=> $update ];
header("Content-Type: application/json; charset=utf-8");
echo json_encode($json);
exit;

?>
