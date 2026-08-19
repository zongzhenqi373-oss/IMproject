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
                    MessageRow(msg, peerNick = p.nick, myNick = vm.myNick.collectAsStateWithLifecycle().value, myId = vm.myId)
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
                        vm.notify("文件传输将在 M7 版本开放")
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
private fun MessageRow(msg: ChatMessage, peerNick: String, myNick: String, myId: Int) {
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
            }
            if (msg.fromMe) {
                Text(
                    when (msg.status) {
                        ChatMessage.Status.SENDING -> "发送中…"
                        ChatMessage.Status.DELIVERED -> "已送达"
                        ChatMessage.Status.OFFLINE_STORED -> "对方离线，已转存"
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
