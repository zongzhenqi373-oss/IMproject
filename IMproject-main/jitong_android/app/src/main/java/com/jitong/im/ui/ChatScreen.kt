package com.jitong.im.ui

import android.graphics.BitmapFactory
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.jitong.im.util.ImageCodec
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.launch

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ChatScreen(vm: MainViewModel) {
    val peer by vm.chatPeer.collectAsStateWithLifecycle()
    val allMessages by vm.messages.collectAsStateWithLifecycle()
    val p = peer ?: return
    val conv = allMessages[p.id].orEmpty()

    val listState = rememberLazyListState()
    // 仅当"最新一条"变化（新消息到底部）时才滚到底；上拉加载更早消息不改变末条 id，不触发滚动，
    // 避免把用户从顶部拽回底部（对齐漫游设计决策 7：简单版不做滚动锚点，允许轻微跳动）
    LaunchedEffect(conv.lastOrNull()?.msgId) {
        if (conv.isNotEmpty()) listState.animateScrollToItem(conv.size - 1)
    }
    // 滚到顶部触发上拉加载更早历史（VM 内部按 hasMore/loading/游标防抖）
    LaunchedEffect(listState, p.id) {
        snapshotFlow { listState.firstVisibleItemIndex }
            .distinctUntilChanged()
            .collect { idx -> if (idx == 0 && conv.isNotEmpty()) vm.loadMoreHistory(p.id) }
    }

    var input by rememberSaveable { mutableStateOf("") }
    var showPanel by remember { mutableStateOf(false) }
    val keyboard = LocalSoftwareKeyboardController.current
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    // 系统相册选择器（Photo Picker，无需存储权限）
    val pickImage = rememberLauncherForActivityResult(
        ActivityResultContracts.PickVisualMedia()
    ) { uri ->
        if (uri != null) {
            scope.launch {
                val compressed = ImageCodec.loadAndCompress(context, uri)
                if (compressed != null) {
                    vm.sendImage(compressed.bytes, compressed.w, compressed.h)
                } else {
                    vm.notify("图片读取失败")
                }
            }
        }
    }

    // SAF 选文件（任意 MIME），交给 vm.sendFile 走上传状态机
    val pickFile = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) vm.sendFile(uri, context.contentResolver)
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Avatar(id = p.id, nick = p.nick, size = 36.dp)
                        Spacer(Modifier.width(10.dp))
                        Column {
                            Text(p.nick)
                            Text(
                                if (p.online) "在线" else "离线",
                                fontSize = 12.sp,
                                color = if (p.online) Color(0xFF07C160) else Color.Gray,
                            )
                        }
                    }
                },
                navigationIcon = {
                    TextButton(onClick = { vm.backToFriends() }) { Text("返回") }
                },
            )
        },
    ) { padding ->
        Column(
            Modifier
                .fillMaxSize()
                .padding(padding)
                .background(Color(0xFFEDEDED)) // 灰底衬托白色对方气泡
                .imePadding(),
        ) {
            LazyColumn(
                state = listState,
                modifier = Modifier
                    .weight(1f)
                    .fillMaxWidth()
                    .padding(horizontal = 12.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                items(conv, key = { it.msgId }) { msg ->
                    MessageRow(msg, peerNick = p.nick, myNick = vm.myNick.collectAsStateWithLifecycle().value, myId = vm.myId, vm = vm)
                }
            }

            // 输入行：文本框 + 「+」面板入口 + 发送
            Row(
                Modifier
                    .fillMaxWidth()
                    .background(Color(0xFFF7F7F7))
                    .padding(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                OutlinedTextField(
                    value = input,
                    onValueChange = { input = it },
                    modifier = Modifier.weight(1f),
                    placeholder = { Text("输入消息…") },
                    maxLines = 3,
                )
                // 「+」号入口（对齐 QQ/微信）：展开 相册/文件 面板
                Box(
                    Modifier
                        .padding(start = 8.dp)
                        .size(40.dp)
                        .clip(RoundedCornerShape(8.dp))
                        .background(Color.White)
                        .clickable {
                            showPanel = !showPanel
                            if (showPanel) keyboard?.hide()
                        },
                    contentAlignment = Alignment.Center,
                ) { Text(if (showPanel) "×" else "＋", fontSize = 22.sp, color = Color(0xFF07C160)) }
                Button(
                    onClick = {
                        vm.send(input)
                        input = ""
                    },
                    modifier = Modifier.padding(start = 8.dp),
                ) { Text("发送") }
            }

            // 「+」展开面板
            if (showPanel) {
                Row(
                    Modifier
                        .fillMaxWidth()
                        .background(Color(0xFFF7F7F7))
                        .padding(16.dp),
                    horizontalArrangement = Arrangement.spacedBy(24.dp),
                ) {
                    PanelItem("相册") {
                        showPanel = false
                        pickImage.launch(
                            PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)
                        )
                    }
                    PanelItem("文件") {
                        showPanel = false
                        pickFile.launch(arrayOf("*/*"))
                    }
                }
            }
        }
    }
}

@Composable
private fun PanelItem(label: String, onClick: () -> Unit) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Box(
            Modifier
                .size(60.dp)
                .clip(RoundedCornerShape(12.dp))
                .background(Color.White)
                .clickable(onClick = onClick),
            contentAlignment = Alignment.Center,
        ) {
            Text(label.take(1), fontSize = 24.sp, color = Color(0xFF07C160))
        }
        Spacer(Modifier.height(4.dp))
        Text(label, fontSize = 12.sp, color = Color.Gray)
    }
}

/** 一条消息：头像 + 气泡（自己靠右绿色，对方靠左白色） */
@Composable
private fun MessageRow(msg: ChatMessage, peerNick: String, myNick: String, myId: Int, vm: MainViewModel) {
    val context = LocalContext.current
    Row(
        Modifier.fillMaxWidth(),
        horizontalArrangement = if (msg.fromMe) Arrangement.End else Arrangement.Start,
        verticalAlignment = Alignment.Top,
    ) {
        if (!msg.fromMe) {
            Avatar(id = msg.peerId, nick = peerNick, size = 40.dp)
            Spacer(Modifier.width(8.dp))
        }
        Column(
            horizontalAlignment = if (msg.fromMe) Alignment.End else Alignment.Start,
        ) {
            when (msg.kind) {
                MsgKind.TEXT -> Box(
                    Modifier
                        .widthIn(max = 260.dp)
                        .background(
                            if (msg.fromMe) Color(0xFF95EC69) else Color.White,
                            RoundedCornerShape(8.dp),
                        )
                        .padding(horizontal = 12.dp, vertical = 8.dp),
                ) { Text(msg.text) }

                MsgKind.IMAGE -> ImageBubble(msg)

                // 文件消息气泡：图标 + 名称 + 大小 + 进度/下载/打开
                MsgKind.FILE -> FileBubble(msg, onDownload = { vm.downloadFile(msg) }, onOpen = {
                    msg.localPath?.let { openFile(context, it, msg.fileName) }
                }, onRetry = { vm.retryFile(msg) })
            }
            if (msg.fromMe) {
                Text(
                    when (msg.status) {
                        ChatMessage.Status.SENDING -> "发送中…"
                        ChatMessage.Status.DELIVERED -> "已送达"
                        ChatMessage.Status.OFFLINE_STORED -> "对方离线，已转存"
                        ChatMessage.Status.FAILED -> "发送失败，点击重试"
                        ChatMessage.Status.RECEIVED -> ""
                    },
                    fontSize = 10.sp,
                    color = Color.Gray,
                )
            }
        }
        if (msg.fromMe) {
            Spacer(Modifier.width(8.dp))
            Avatar(id = myId, nick = myNick, size = 40.dp)
        }
    }
}

/**
 * 固定尺寸文件卡片：左侧文件图标，右侧文件名及大小/操作状态。
 *
 * 下载防重入：downloadFile 内部对 .part 文件用追加流写入，无二次点击去重逻辑，
 * 因此这里在 UI 层做守卫——仅当"未下载 且 非下载中（status != SENDING）"时才允许触发 onDownload，
 * 下载中（对方视角复用 SENDING 表示"进行中"）点击直接忽略，避免并发打开第二个追加流。
 */
@Composable
private fun FileBubble(msg: ChatMessage, onDownload: () -> Unit, onOpen: () -> Unit, onRetry: () -> Unit) {
    val downloaded = msg.localPath != null && !msg.localPath!!.endsWith(".part")
    val downloading = msg.status == ChatMessage.Status.SENDING
    val failed = msg.fromMe && msg.status == ChatMessage.Status.FAILED
    val total = ((msg.fileSize + FILE_CHUNK - 1) / FILE_CHUNK).toInt().coerceAtLeast(1)
    val progress = (msg.transferred * 100 / total).coerceIn(0, 100)
    val actionText = when {
        downloading -> "传输中 $progress%"
        failed -> "点击重试"
        downloaded -> "点击打开"
        !msg.fromMe -> "点击下载"
        else -> "已发送"
    }
    val actionColor = when {
        failed -> Color(0xFFD93025)
        downloaded || !msg.fromMe -> Color(0xFF07C160)
        else -> Color.Gray
    }

    Row(
        Modifier
            .width(260.dp)
            .height(76.dp)
            .background(if (msg.fromMe) Color(0xFF95EC69) else Color.White, RoundedCornerShape(8.dp))
            .clickable {
                when {
                    failed -> onRetry()
                    downloaded -> onOpen()
                    !msg.fromMe && !downloading -> onDownload()
                    // 下载中或己方发送记录：忽略点击，防止重复下载/无意义交互
                }
            }
            .padding(12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier
                .size(48.dp)
                .background(Color.White.copy(alpha = 0.72f), RoundedCornerShape(8.dp)),
            contentAlignment = Alignment.Center,
        ) {
            Text("📄", fontSize = 27.sp)
        }
        Spacer(Modifier.width(12.dp))
        Column(
            Modifier.weight(1f),
            verticalArrangement = Arrangement.Center,
        ) {
            Text(
                text = msg.fileName.ifBlank { "未命名文件" },
                fontSize = 14.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Spacer(Modifier.height(5.dp))
            Text(
                text = "${humanSize(msg.fileSize)} · $actionText",
                fontSize = 11.sp,
                color = actionColor,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

private const val FILE_CHUNK = 256 * 1024
private fun humanSize(b: Long): String = when {
    b >= 1 shl 20 -> "%.1f MB".format(b / 1048576.0)
    b >= 1 shl 10 -> "%.1f KB".format(b / 1024.0)
    else -> "$b B"
}
private fun openFile(context: android.content.Context, path: String, name: String) {
    runCatching {
        val uri = if (path.startsWith("content://")) {
            android.net.Uri.parse(path)
        } else {
            androidx.core.content.FileProvider.getUriForFile(
                context, "${context.packageName}.fileprovider", java.io.File(path),
            )
        }
        val mime = context.contentResolver.getType(uri)
            ?: java.net.URLConnection.guessContentTypeFromName(name)
            ?: "*/*"
        val intent = android.content.Intent(android.content.Intent.ACTION_VIEW)
            .setDataAndType(uri, mime)
            .addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)
        context.startActivity(intent)
    }
        .onFailure { android.widget.Toast.makeText(context, "无可打开该文件的应用", android.widget.Toast.LENGTH_SHORT).show() }
}

@Composable
private fun ImageBubble(msg: ChatMessage) {
    val bitmap = remember(msg.msgId) {
        msg.imageBytes?.let { BitmapFactory.decodeByteArray(it, 0, it.size)?.asImageBitmap() }
    }
    if (bitmap == null) {
        Text("[图片无法显示]", color = Color.Gray, fontSize = 12.sp)
        return
    }
    val ratio = if (msg.imgW > 0 && msg.imgH > 0) {
        msg.imgW.toFloat() / msg.imgH.toFloat()
    } else {
        bitmap.width.toFloat() / bitmap.height.toFloat()
    }
    Image(
        bitmap = bitmap,
        contentDescription = "图片消息",
        contentScale = ContentScale.Crop,
        modifier = Modifier
            .widthIn(max = 220.dp)
            .aspectRatio(ratio.coerceIn(0.4f, 2.5f))
            .clip(RoundedCornerShape(8.dp))
            .background(Color.White),
    )
}
